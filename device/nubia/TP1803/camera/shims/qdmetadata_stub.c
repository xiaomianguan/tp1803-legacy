/* libqdMetaData replacement. CamX and the OMX video components annotate QTI
 * gralloc buffers with display metadata (geometry, colour, interlace...).
 * Our gralloc has no metadata buffer: sets are accepted and dropped, gets
 * report "not present" so callers fall back to their defaults. */
#include <errno.h>

int setMetaData(void *handle, int paramType, void *param)
{
    (void)handle; (void)paramType; (void)param;
    return 0;
}

int getMetaData(void *handle, int paramType, void *param)
{
    (void)handle; (void)paramType; (void)param;
    return -EINVAL;
}

int clearMetaData(void *handle, int paramType)
{
    (void)handle; (void)paramType;
    return 0;
}
