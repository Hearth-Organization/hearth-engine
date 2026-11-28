/* Hearth Engine's bridge to its Rust parts (hearth-rust/hearth-ffi). Strings out belong to the object that gave
 * them and stay valid until the next call on it. Nothing here throws or aborts: mistakes come back as text. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hearth Script */
typedef struct HsEngine HsEngine;
/* A game function: its name and its values (a JSON list) in; {"ok": value} or {"error": "why"} out (valid until
 * the next call). */
typedef const char* (*HsNative)(void* user, const char* name, const char* args_json);

HsEngine* hs_new(const char* script_name);
void hs_free(HsEngine* e);
void hs_register(HsEngine* e, const char* name, HsNative f, void* user);
const char* hs_load(HsEngine* e, const char* source);                     /* null, or the mistake */
const char* hs_fire(HsEngine* e, const char* event, const char* args_json); /* null, or the mistake */
bool hs_listens(const HsEngine* e, const char* event);
const char* hs_take_output(HsEngine* e);                                  /* what say() wrote, one line each */

/* Hearth Detail (Nanite-style detail) */
typedef struct HdDetail HdDetail;
HdDetail* hd_open(const char* path); /* .glb, .obj or .hdetail; null: see hd_last_error */
const char* hd_last_error(void);
void hd_free(HdDetail* d);
uint32_t hd_vertices(const HdDetail* d, float* out12); /* pos xyz, normal xyz, uv, tangent xyzw; null to count */
void hd_bounds(const HdDetail* d, float* out6);         /* min xyz, max xyz */
bool hd_select(HdDetail* d, float cx, float cy, float cz, float pixels_per_unit, float max_pixels); /* changed? */
const uint32_t* hd_selected(const HdDetail* d, uint32_t* count);
const char* hd_describe(HdDetail* d);

#ifdef __cplusplus
}
#endif
