#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <errno.h>
#include <stdint.h>

#define FB_COUNT 4
#define FB_MAX_LENGTH (2048 * 2048 * 4)         // framebuffer最大长度

// 帧缓冲区信息
struct livision_fb_header_t {
    int width;              // 帧缓冲区宽度
    int height;             // 帧缓冲区高度
    int bpp;                // 每个像素的字节数，支持3（无GL_RGB）或4（GL_RGBA）
    int frame_valid;        // 帧有效标志（由Flutter清零，C语言/Python客户端设置为1）
};

// 控制共享内存内容
struct livision_ctl_t {
    int valid;                                      // 共享内存是否有效，用于保证单次初始化
    sem_t signal;                                   // 条件信号量，C语言/Python客户端在设置帧有效标志后释放，Flutter首先取得signal，
                                                    // **循环检查**frame_valid是否为1，若是则加锁mutex，读取共享内存，否则继续尝试取得signal
                                                    // （可能出现C客户端多次放置buffer但flutter未及时获取的情况）
    sem_t mutex;                                    // 互斥信号量，当修改/读取共享内存时均需加锁
    struct livision_fb_header_t fbs[FB_COUNT];      // 帧缓冲区头部信息
};

struct livision_ctl_t* map_shm_ctl(void);
void unmap_shm_ctl(struct livision_ctl_t* controller);

uint8_t* map_framebuffer(int id);
void set_framebuffer_data(struct livision_ctl_t* controller, int id, int width, int height, int bpp, void* dst, const void* src);
void unmap_framebuffer(uint8_t* fb);
