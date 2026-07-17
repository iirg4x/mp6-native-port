/* MP6 native port -- field-wise big-endian marshal for the three
 * persisted save-box structs.
 *
 * The memory-card box payload (game/saveload.c, SAVE_BOX_* layout) is a raw
 * byte image of GW_COMMON + GW_SYSTEM + GW_PLAYER[GW_PLAYER_MAX] exactly as
 * the GameCube CPU stored it -- BIG-endian scalars, MWCC MSB-first bitfield
 * packing. Console/Dolphin saves must keep that on-card format byte-true
 * (so cards keep interchanging with Dolphin), which means the native build
 * cannot memcpy the little-endian in-memory structs to/from the buffer:
 * every multi-byte field would land byte-swapped (the user-visible symptom:
 * a save tag showing garbage star/coin values instead of the save's real
 * ones), and every bitfield would land bit-reversed within its unit
 * (MWCC allocates from the MSB down, clang/gcc-LE from the LSB up -- a
 * plain byte swap can NOT fix that part, which is why this is a field-wise
 * marshal and not an in-place swapper).
 *
 * platform/os/save_endian.c implements these; the ONLY call sites are the
 * six struct<->saveBuf memcpy boundaries in the patched game/saveload.c
 * (SLCommonSet / SLCommonSaveCopy / SLBoardSave write-side, SLCommonLoad /
 * SLCommonLoadCopy / SLBoardLoad load-side). Everything else that touches
 * saveBuf is endian-safe as-is: 4-char "SAVE"/"EMPT" magics (strncmp),
 * byte-wise checksums (compare sites compose the u16 big-endian),
 * buffer-to-buffer box copies (BE both sides), and the byte-wise
 * comment/date/icon writers.
 *
 * Direction discipline: *_from_be = buffer stays BE, struct becomes native
 * (call AFTER locating the box); *_to_be = compose the BE image directly
 * into the buffer from the native struct (the card image never holds a
 * native-endian word).
 */
#ifndef MP6_SAVE_ENDIAN_H
#define MP6_SAVE_ENDIAN_H

struct GwCommon_s;
struct GwSystem_s;
struct GwPlayer_s;

void mp6_save_common_from_be(struct GwCommon_s *dst, const void *src);
void mp6_save_common_to_be(void *dst, const struct GwCommon_s *src);
void mp6_save_system_from_be(struct GwSystem_s *dst, const void *src);
void mp6_save_system_to_be(void *dst, const struct GwSystem_s *src);
void mp6_save_player_from_be(struct GwPlayer_s *dst, const void *src);
void mp6_save_player_to_be(void *dst, const struct GwPlayer_s *src);

#endif /* MP6_SAVE_ENDIAN_H */
