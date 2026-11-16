/* libcdsprpc replacement for CamX: FastRPC to the compute DSP is only used for
 * optional offload (face detection, HVX nodes). The real library depends on
 * HIDL services Android 6 doesn't have; report "unsupported" instead. */
#include <stddef.h>
#include <stdint.h>

#define AEE_EUNSUPPORTED 20

int remote_handle_open(const char *name, uint32_t *ph) { (void)name; (void)ph; return AEE_EUNSUPPORTED; }
int remote_handle_invoke(uint32_t h, uint32_t sc, void *pra) { (void)h; (void)sc; (void)pra; return AEE_EUNSUPPORTED; }
int remote_handle_close(uint32_t h) { (void)h; return AEE_EUNSUPPORTED; }
int remote_handle64_open(const char *name, uint64_t *ph) { (void)name; (void)ph; return AEE_EUNSUPPORTED; }
int remote_handle64_invoke(uint64_t h, uint32_t sc, void *pra) { (void)h; (void)sc; (void)pra; return AEE_EUNSUPPORTED; }
int remote_handle64_close(uint64_t h) { (void)h; return AEE_EUNSUPPORTED; }
void *rpcmem_alloc(int heapid, uint32_t flags, int size) { (void)heapid; (void)flags; (void)size; return NULL; }
void rpcmem_free(void *po) { (void)po; }
