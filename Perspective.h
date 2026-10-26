#ifndef PERSPECTIVE_H_
#define PERSPECTIVE_H_

#include "main.h"                       /* CYT4BB7 工程改成 #include "main_0.h" */

#define USED_ROW            MT9V03X_H   /* 源图行数 */
#define USED_COL            MT9V03X_W   /* 源图列数 */
#define RESULT_ROW          MT9V03X_H   /* 输出图行数 */
#define RESULT_COL          MT9V03X_W   /* 输出图列数 */

/* 编译期三选一，只有被选中的方案会分配内存 */
#define PERSPECTIVE_MODE_TABLE          0   /* 指针表       90120 B */
#define PERSPECTIVE_MODE_OFFSET         1   /* int16 偏移表 45060 B */
#define PERSPECTIVE_MODE_U8             2   /* uint8 压缩表 22800 B */

#define PERSPECTIVE_MODE                0

#if   (PERSPECTIVE_MODE == PERSPECTIVE_MODE_TABLE)
    #define PERSPECTIVE_MODE_NAME       "TABLE"
#elif (PERSPECTIVE_MODE == PERSPECTIVE_MODE_OFFSET)
    #define PERSPECTIVE_MODE_NAME       "OFFSET"
#elif (PERSPECTIVE_MODE == PERSPECTIVE_MODE_U8)
    #define PERSPECTIVE_MODE_NAME       "U8"
#else
    #error "PERSPECTIVE_MODE invalid"
#endif


#if (PERSPECTIVE_MODE == PERSPECTIVE_MODE_OFFSET)

/* 存相对图像首字节的偏移，越界格写 0 */
extern __attribute__((aligned(4))) int16 PerImg_ofs[RESULT_ROW][RESULT_COL];

#define PERSPECTIVE_BASE            (&mt9v03x_image[0][0])
#define PERSPECTIVE_TABLE_SIZE      ((uint32)sizeof(PerImg_ofs))
#define PERSPECTIVE_PIXEL(row, col) ((uint32)(*(PERSPECTIVE_BASE + PerImg_ofs[(row)][(col)])))

#elif (PERSPECTIVE_MODE == PERSPECTIVE_MODE_U8)

/* h10 = h20 = 0，y 只跟行有关，x 落在 0..187，所以表拆成行基址 + 每格 x */
extern uint8  PerImg_xcol[RESULT_ROW][RESULT_COL];
extern uint8 *PerImg_rowbase[RESULT_ROW];
extern uint8  PerImg_rvalid[RESULT_ROW];

#define PERSPECTIVE_TABLE_SIZE      ((uint32)(sizeof(PerImg_xcol) + sizeof(PerImg_rowbase) + sizeof(PerImg_rvalid)))

/* 整行无效返回 0，行内越界在 Init 里已钳到行首 */
#define PERSPECTIVE_PIXEL(row, col)                                             \
    ((0 == PerImg_rvalid[(row)])                                                \
        ? 0u                                                                    \
        : (uint32)PerImg_rowbase[(row)][PerImg_xcol[(row)][(col)]])

#else

extern __attribute__((aligned(4))) uint8_t *PerImg_ip[RESULT_ROW][RESULT_COL];

#define PERSPECTIVE_TABLE_SIZE      ((uint32)sizeof(PerImg_ip))
#define PERSPECTIVE_PIXEL(row, col) (*(PerImg_ip[(row)][(col)]))

#endif


void   ImagePerspective_Init(void);
uint32 perspective_sum(void);


#endif /* PERSPECTIVE_H_ */
