#include "livision_shm.h"

#define SHM_FILE_CTL "/livision_ctl"
#define SHM_SIZE_CTL sizeof(struct livision_ctl_t)
#define SHM_FILE_FRAME "/livision_frame_%d"

struct livision_ctl_t* map_shm_ctl(void)
{
    int fd = shm_open(SHM_FILE_CTL, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("open");
        return NULL;
    }

    int success = ftruncate(fd, SHM_SIZE_CTL);
    if (success != 0) {
        perror("ftruncate");
        return NULL;
    }

    struct livision_ctl_t* controller = mmap(NULL, SHM_SIZE_CTL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (controller == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    close(fd);
    // 首次创建共享内存区域时，初始化信号量
    if (controller->valid == 0) {
        controller->valid = 1;
        sem_init(&controller->signal, 1, 0);
        sem_init(&controller->mutex, 1, 1);
    }
    return controller;
}

void unmap_shm_ctl(struct livision_ctl_t* controller)
{
    if (controller != NULL) {
        // 关闭共享内存，但不删除
        munmap(controller, SHM_SIZE_CTL);
    }
}

static const char* get_framebuffer_name(int id)
{
    static char name[32];
    snprintf(name, sizeof(name), SHM_FILE_FRAME, id);
    return name;
}

/**
 * @param controller 缓冲区控制器
 * @param fb 分配请求，调用者需填写除data外的数据
 * @return 0为成功，同时通过fb返回对应framebuffer地址，否则返回errno
 */

uint8_t* map_framebuffer(int id)
{
    if (id >= FB_COUNT) return NULL;

    int fd = shm_open(get_framebuffer_name(id), O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("open");
        return NULL;
    }

    int success = ftruncate(fd, FB_MAX_LENGTH);
    if (success != 0) {
        perror("ftruncate");
        return NULL;
    }

    uint8_t* data = mmap(NULL, FB_MAX_LENGTH, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    close(fd);

    return data;
}


void set_framebuffer_data(struct livision_ctl_t* controller, int id, int width, int height, int bpp, void* dst, const void* src)
{
    if (width * height * bpp > FB_MAX_LENGTH) return;
    sem_wait(&controller->mutex);
    memcpy(dst, src, width * height * bpp);
    controller->fbs[id].bpp = bpp;
    controller->fbs[id].width = width;
    controller->fbs[id].height = height;
    controller->fbs[id].frame_valid = 1;
    sem_post(&controller->signal);
    sem_post(&controller->mutex);
}

void unmap_framebuffer(uint8_t* fb)
{
    munmap(fb, FB_MAX_LENGTH);
}
