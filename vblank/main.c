#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "drm_fourcc.h"
#include "xf86drm.h"
#include "xf86drmMode.h"

// taken from https://gist.github.com/mokumus/bdd9d4fa837345f01b35e0cd03d67f35
char * timestamp();
#define print_log(f_, ...) printf("%s ", timestamp()), printf((f_), ##__VA_ARGS__), printf("\n")
// end

struct drm_wrap {
    drmModeRes *drm_res;
    drmModeConnector *connector;
    drmModeEncoder *enc;
    drmModeCrtc *crtc;
    int drm_fd;
    int crtc_index;
};

struct dumb_fb {
    uint32_t format;
    uint32_t width, height, stride, size;
    uint32_t handle;
    uint32_t id;
};

// taken from https://gist.github.com/mokumus/bdd9d4fa837345f01b35e0cd03d67f35
char * timestamp(){
    time_t now = time(NULL); 
    char * time = asctime(gmtime(&now));
    time[strlen(time)-1] = '\0';    // Remove \n
    return time;
}
// end

struct drm_wrap *open_drm_wrap(char *path) {

    struct drm_wrap *wrap = malloc(sizeof(struct drm_wrap));
    int i, err;

    wrap->drm_fd = open(path, O_RDWR | O_CLOEXEC);
    if (wrap->drm_fd < 0) {
        printf("Could not open drm device: %s", strerror(wrap->drm_fd));
        return NULL;
    }

    err = drmSetClientCap(wrap->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (err < 0) {
        printf("Could not set DRM_CLIENT_CAP_ATOMIC: %s", strerror(err));
        return NULL;
    }

    err = drmSetClientCap(wrap->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (err < 0) {
        printf("Could not set DRM_CLIENT_CAP_UNIVERSAL_PLANES: %s", strerror(err));
        return NULL;
    }

    wrap->drm_res = drmModeGetResources(wrap->drm_fd);
    if (wrap->drm_res == NULL) {
        printf("Could not get resources");
        return NULL;
    }

    for (i = 0; i < wrap->drm_res->count_connectors; i++) {
        wrap->connector = drmModeGetConnector(wrap->drm_fd, wrap->drm_res->connectors[i]);
        if (wrap->connector->connection != DRM_MODE_CONNECTED) {
            drmModeFreeConnector(wrap->connector);
        }
    }

    if (wrap->connector != NULL) {
        printf("Found connector %d\n", wrap->connector->connector_id);
        wrap->enc = drmModeGetEncoder(wrap->drm_fd, wrap->connector->encoder_id);
        if (wrap->enc != NULL) {
            printf("Found encoder %d\n", wrap->enc->encoder_id);
            wrap->crtc = drmModeGetCrtc(wrap->drm_fd, wrap->enc->crtc_id);
            if (wrap->crtc != NULL) {
                printf("Found crtc %d\n", wrap->crtc->crtc_id);

                for (i = 0; i < wrap->drm_res->count_crtcs; i++) {
                    if (wrap->drm_res->crtcs[i] == wrap->crtc->crtc_id) {
                        wrap->crtc_index = i;
                        break;
                    }
                }
            }
        }
    }

    return wrap;
}

void close_drm_wrap(struct drm_wrap *wrap) {
    if (wrap->crtc != NULL) {
        drmModeFreeCrtc(wrap->crtc);
    }

    if (wrap->enc != NULL) {
        drmModeFreeEncoder(wrap->enc);
    }

    if (wrap->connector != NULL) {
        drmModeFreeConnector(wrap->connector);
    }

    if (wrap->drm_res != NULL) {
        drmModeFreeResources(wrap->drm_res);
    }

    close(wrap->drm_fd);
}

void *
dumb_fb_map(struct dumb_fb *fb, int drm_fd) {
    int ret;

    struct drm_mode_map_dumb map = {.handle = fb->handle};
    ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    if (ret < 0) {
        return MAP_FAILED;
    }

    return mmap(0, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd,
                map.offset);
}

void
dumb_fb_fill(struct dumb_fb *fb, int drm_fd, uint32_t color) {
    uint32_t *data;
    size_t i;

    data = dumb_fb_map(fb, drm_fd);
    if (data == MAP_FAILED) {
        return;
    }

    printf("Writing data to fb...\n");

    for (i = 0; i < fb->size / sizeof(uint32_t); i++) {
        data[i] = color;
    }

    munmap(data, fb->size);
}

bool
dumb_fb_init(struct dumb_fb *fb, int drm_fd, uint32_t format, uint32_t width,
             uint32_t height) {
    int ret;
    uint32_t fb_id;

    struct drm_mode_create_dumb create = {
            .width = width,
            .height = height,
            .bpp = 32,
            .flags = 0,
    };
    ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
    if (ret < 0) {
        printf("creating dumb buffer failed: %d\n", ret);
        return false;
    }

    uint32_t handles[4] = {create.handle};
    uint32_t strides[4] = {create.pitch};
    uint32_t offsets[4] = {0};

    ret = drmModeAddFB2(drm_fd, width, height, format, handles, strides,
                        offsets, &fb_id, 0);
    if (ret < 0) {
        printf("add fb2 failed %d\n", ret);
        return false;
    }

    fb->width = width;
    fb->height = height;
    fb->stride = create.pitch;
    fb->size = create.size;
    fb->handle = create.handle;
    fb->id = fb_id;
    return true;
}

static uint32_t get_plane_id(int drm_fd, int crtc, int type)
{
    drmModePlaneResPtr plane_resources;
    uint32_t i, j, ret = -1;
    int found_plane = 0;

    plane_resources = drmModeGetPlaneResources(drm_fd);
    if (!plane_resources)
    {
        printf("drmModeGetPlaneResources failed: %s\n", strerror(errno));
        return -1;
    }

    for (i = 0; (i < plane_resources->count_planes) && !found_plane; i++)
    {
        uint32_t id = plane_resources->planes[i];
        drmModePlanePtr plane = drmModeGetPlane(drm_fd, id);
        if (!plane)
        {
            printf("drmModeGetPlane(%u) failed: %s\n", id, strerror(errno));
            continue;
        }

        if (plane->possible_crtcs & (1 << crtc))
        {
            drmModeObjectPropertiesPtr props =
                drmModeObjectGetProperties(drm_fd, id, DRM_MODE_OBJECT_PLANE);

            for (j = 0; j < props->count_props; j++)
            {
                drmModePropertyPtr p =
                    drmModeGetProperty(drm_fd, props->props[j]);

                if ((strcmp(p->name, "type") == 0) &&
                    (props->prop_values[j] == type))
                {

                    printf("Found plane of type %d\n", type);
                    ret = id;
                    found_plane = 1;
                }

                drmModeFreeProperty(p);
            }

            drmModeFreeObjectProperties(props);
        }

        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(plane_resources);

    return ret;
}

int main()
{
    struct drm_wrap *wrap = open_drm_wrap("/dev/dri/card0");

    // on width = 1, every multiple of 8 in height will cause the issue
    int err = 0, buffer_width = 1, buffer_height = 8;
    struct dumb_fb buffer = {0};
    uint32_t format = DRM_FORMAT_ARGB8888;

    if (!dumb_fb_init(&buffer, wrap->drm_fd, format, buffer_width, buffer_height))
    {
        printf("failed to create framebuffer\n");
        goto cleanup;
    }
    
    printf("Created FB %d with size %dx%d\n", buffer.id, buffer_width, buffer_height);
    dumb_fb_fill(&buffer, wrap->drm_fd, 0xFF00FF00); // blue

    // plane type doesn't matter
    uint32_t plane_id = get_plane_id(wrap->drm_fd, wrap->crtc_index, DRM_PLANE_TYPE_PRIMARY);
    if (plane_id < 0) {
        printf("failed to create plane\n");
        goto cleanup;
    }

    printf("\n");

    print_log("Updating first plane to width of 1");
    err = drmModeSetPlane(wrap->drm_fd, plane_id,
                            wrap->crtc->crtc_id, buffer.id, 0,
                            8, 8, buffer_width, buffer_height,
                            0 << 16, 0 << 16, buffer_width << 16, buffer_height << 16); // src
    if (err)
    {
        print_log("error setting plane: %d", err);
        goto cleanup;
    }

    print_log("Update done.\n");

    int c = getchar();

    char str[4];
    sprintf(str, "%d", c);
    printf("Result: %s\n", str);

cleanup:
    close_drm_wrap(wrap);

    return err;
}
