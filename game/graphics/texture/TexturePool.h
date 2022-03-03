#pragma once

#include <array>
#include <memory>
#include <string>
#include <mutex>
#include <optional>
#include <unordered_map>
#include "common/common_types.h"
#include "game/graphics/texture/TextureConverter.h"
#include "common/util/Serializer.h"

// verify all texture lookups.
// will make texture lookups slower and likely caused dropped frames when loading
constexpr bool EXTRA_TEX_DEBUG = false;

// sky, cloud textures
constexpr int SKY_TEXTURE_VRAM_ADDRS[2] = {8064, 8096};

/*!
 * PC Port Texture System
 *
 * The main goal of this texture system is to support fast lookup textures by VRAM address
 * (sometimes called texture base pointer or TBP). The lookup ends up being a single read from
 * an array - no pointer chasing required.
 *
 * The TIE/TFRAG background renderers use their own more efficient system for this.
 * This is only used for renderers that interpret GIF data (sky, eyes, generic, merc, direct,
 * sprite).
 *
 * Some additional challenges:
 * - Some textures are generated by rendering to a texture (eye, sky)
 * - The game may try to render things before their textures have been loaded.  This is a "bug" in
 *   the original game, but can't be seen most of the time because the objects are often hidden.
 * - We preconvert PS2-format textures and store them in the FR3 level asset files. But the game may
 *   try to use the textures before the PC port has finished loading them.
 * - The game may copy textures from one location in VRAM to another
 * - The game may store two texture on top of each other in some formats (only the font). The PS2's
 *   texture formats allow you to do this if you use the right pair formats.
 * - The same texture may appear in multiple levels, both of which can be loaded at the same time.
 *   The two levels can unload in either order, and the remaining level should be able to use the
 *   texture.
 * - Some renderers need to access the actual texture data on the CPU.
 * - We don't want to load all the textures into VRAM at the same time.
 *
 * But, we have a few assumptions we make to simplify things:
 * - Two textures with the same "combined name" are always identical data. (This is verified by the
 *   decompiler). So we can use the name as an ID for the texture.
 * - The game will remove all references to textures that belong to an unloaded level, so once the
 *   level is gone, we can forget its textures.
 * - The number of times a texture is duplicated (both in VRAM, and in loaded levels) is small
 *
 * Unlike the first version of the texture system, our approach is to load all the textures to
 * the GPU during loading.
 *
 * This approach has three layers:
 * - A VRAM entry (TextureReference), which refers to a GpuTexture
 * - A GpuTexture, which represents an in-game texture, and refers to all loaded instances of it
 * - Actual texture data
 *
 * Note that the VRAM entries store the GLuint for the actual texture reference inline, so texture
 * lookups during drawing are very fast. The time to set up and maintain all these links only
 * happens during loading, and it's insignificant compared to reading from the hard drive or
 * unpacking/uploading meshes.
 *
 * The loader will inform us when things are added/removed.
 * The game will inform us when it uploads to VRAM
 */

/*!
 * The lowest level reference to texture data.
 */
struct TextureData {
  u64 gl = -1;               // the OpenGL texture ID
  const u8* data = nullptr;  // pointer to texture data (owned by the loader)
};

/*!
 * This represents a unique in-game texture, including any instances of it that are loaded.
 * It's possible for there to be 0 instances of the texture loaded yet.
 */
struct GpuTexture {
  std::string page_name;
  std::string name;

  // all the currently loaded copies of this texture
  std::vector<TextureData> gpu_textures;

  // the vram addresses that contain this texture
  std::vector<u32> slots;

  // the vram address that contain this texture, stored in mt4hh format
  std::vector<u32> mt4hh_slots;

  // our "combo id", containing the tpage and texture ID
  u32 combo_id = -1;

  // texture dimensions
  u16 w, h;

  // set to true if we have no copies of the texture, and we should use a placeholder
  bool is_placeholder = false;

  // set to true if we are part of the textures in GAME.CGO that are always loaded.
  // for these textures, the pool can assume that we are never a placeholder.
  bool is_common = false;

  // the size of our data, in bytes
  u32 data_size() const { return 4 * w * h; }

  // get a pointer to our data, or nullptr if we are a placeholder.
  const u8* get_data_ptr() const {
    if (is_placeholder) {
      return nullptr;
    } else {
      return gpu_textures.at(0).data;
    }
  }

  // add or remove a VRAM reference to this texture
  void remove_slot(u32 slot);
  void add_slot(u32 slot);
};

/*!
 * A VRAM slot.
 * If the source is nullptr, the game has not loaded anything to this address.
 * If the game has loaded something, but the loader hasn't loaded the converted texture, the
 * source will be non-null and the gpu_texture will be a placeholder that is safe to use.
 */
struct TextureVRAMReference {
  u64 gpu_texture = -1;  // the OpenGL texture to use when rendering.
  GpuTexture* source = nullptr;
};

/*!
 * A texture provided by the loader.
 */
struct TextureInput {
  std::string page_name;
  std::string name;
  u64 gpu_texture = -1;
  bool common = false;
  u32 combo_id = -1;
  const u8* src_data;
  u16 w, h;
};

/*!
 * The in-game texture type.
 */
struct GoalTexture {
  s16 w;
  s16 h;
  u8 num_mips;
  u8 tex1_control;
  u8 psm;
  u8 mip_shift;
  u16 clutpsm;
  u16 dest[7];
  u16 clut_dest;
  u8 width[7];
  u32 name_ptr;
  u32 size;
  float uv_dist;
  u32 masks[3];

  s32 segment_of_mip(s32 mip) const {
    if (2 >= num_mips) {
      return num_mips - mip - 1;
    } else {
      return std::max(0, 2 - mip);
    }
  }
};

static_assert(sizeof(GoalTexture) == 60, "GoalTexture size");
static_assert(offsetof(GoalTexture, clutpsm) == 8);
static_assert(offsetof(GoalTexture, clut_dest) == 24);

/*!
 * The in-game texture page type.
 */
struct GoalTexturePage {
  struct Seg {
    u32 block_data_ptr;
    u32 size;
    u32 dest;
  };
  u32 file_info_ptr;
  u32 name_ptr;
  u32 id;
  s32 length;  // texture count
  u32 mip0_size;
  u32 size;
  Seg segment[3];
  u32 pad[16];
  // start of array.

  std::string print() const;

  bool try_copy_texture_description(GoalTexture* dest,
                                    int idx,
                                    const u8* memory_base,
                                    const u8* tpage,
                                    u32 s7_ptr) {
    u32 ptr;
    memcpy(&ptr, tpage + sizeof(GoalTexturePage) + 4 * idx, 4);
    if (ptr == s7_ptr) {
      return false;
    }
    memcpy(dest, memory_base + ptr, sizeof(GoalTexture));
    return true;
  }
};

/*!
 * The main texture pool.
 * Moving textures around should be done with locking. (the game EE thread and the loader run
 * simultaneously)
 *
 * Lookups can be done without locking.
 * It is safe for renderers to use textures without worrying about locking - OpenGL textures
 * themselves are only removed from the rendering thread.
 *
 * There could be races with the game doing texture uploads and doing texture lookups, but these
 * races are harmless. If there's an actual in-game race condition, the exact texture you get may be
 * unknown, but you will get a valid texture.
 *
 * (note that the above property is only true because we never make a VRAM slot invalid after
 *  it has been loaded once)
 */
class TexturePool {
 public:
  TexturePool();
  void handle_upload_now(const u8* tpage, int mode, const u8* memory_base, u32 s7_ptr);
  GpuTexture* give_texture(const TextureInput& in);
  GpuTexture* give_texture_and_load_to_vram(const TextureInput& in, u32 vram_slot);
  void unload_texture(const std::string& name, u64 id);

  /*!
   * Look up an OpenGL texture by vram address. Return std::nullopt if the game hasn't loaded
   * anything to this address.
   */
  std::optional<u64> lookup(u32 location) {
    auto& t = m_textures[location];
    if (t.source) {
      if constexpr (EXTRA_TEX_DEBUG) {
        if (t.source->is_placeholder) {
          ASSERT(t.gpu_texture == m_placeholder_texture_id);
        } else {
          bool fnd = false;
          for (auto& tt : t.source->gpu_textures) {
            if (tt.gl == t.gpu_texture) {
              fnd = true;
              break;
            }
          }
          ASSERT(fnd);
        }
      }
      return t.gpu_texture;
    } else {
      return {};
    }
  }

  /*!
   * Look up a game texture by VRAM address. Will be nullptr if the game hasn't loaded anything to
   * this address.
   *
   * You should probably not use this to lookup textures that could be uploaded with
   * handle_upload_now.
   */
  GpuTexture* lookup_gpu_texture(u32 location) { return m_textures[location].source; }
  std::optional<u64> lookup_mt4hh(u32 location);
  u64 get_placeholder_texture() { return m_placeholder_texture_id; }
  void draw_debug_window();
  void relocate(u32 destination, u32 source, u32 format);
  void draw_debug_for_tex(const std::string& name, GpuTexture* tex, u32 slot);
  const std::array<TextureVRAMReference, 1024 * 1024 * 4 / 256> all_textures() const {
    return m_textures;
  }
  void move_existing_to_vram(GpuTexture* tex, u32 slot_addr);

  std::mutex& mutex() { return m_mutex; }

 private:
  void refresh_links(GpuTexture& texture);
  GpuTexture* get_gpu_texture_for_slot(const std::string& name, u32 slot);

  char m_regex_input[256] = "";
  std::array<TextureVRAMReference, 1024 * 1024 * 4 / 256> m_textures;
  struct Mt4hhTexture {
    TextureVRAMReference ref;
    u32 slot;
  };
  std::vector<Mt4hhTexture> m_mt4hh_textures;

  std::vector<u32> m_placeholder_data;
  u64 m_placeholder_texture_id = 0;

  std::unordered_map<std::string, GpuTexture> m_loaded_textures;

  std::mutex m_mutex;
};