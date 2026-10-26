#include "Perspective.h"

/* 标定矩阵。h10 = h20 = 0，分母只跟行有关，U8 方案依赖这一点 */
double change_un_Mat[3][3] = { {0.224880,-0.268106,12.848148},{-0.000000,0.024928,5.810294},{-0.000000,-0.002784,0.350978} };

#define PER_IMG     mt9v03x_image


/* 表定义：只有被选中的方案分配内存 */
#if (PERSPECTIVE_MODE == PERSPECTIVE_MODE_OFFSET)
__attribute__((aligned(4))) int16 PerImg_ofs[RESULT_ROW][RESULT_COL];
#elif (PERSPECTIVE_MODE == PERSPECTIVE_MODE_U8)
uint8  PerImg_xcol[RESULT_ROW][RESULT_COL];
uint8 *PerImg_rowbase[RESULT_ROW];
uint8  PerImg_rvalid[RESULT_ROW];
#else
__attribute__((aligned(4))) uint8_t *PerImg_ip[RESULT_ROW][RESULT_COL];
static uint8_t BlackColor = 0;
#endif


/* 指针表初始化：逐格存 &image[y][x]，越界指向恒为 0 的静态字节 */
#if (PERSPECTIVE_MODE == PERSPECTIVE_MODE_TABLE)

void ImagePerspective_Init(void)
{
    for (int i = 0; i < RESULT_COL; i++)
    {
        for (int j = 0; j < RESULT_ROW; j++)
        {
            int local_x = (int)((change_un_Mat[0][0] * i
                    + change_un_Mat[0][1] * j + change_un_Mat[0][2])
                    / (change_un_Mat[2][0] * i + change_un_Mat[2][1] * j
                            + change_un_Mat[2][2]));
            int local_y = (int)((change_un_Mat[1][0] * i
                    + change_un_Mat[1][1] * j + change_un_Mat[1][2])
                    / (change_un_Mat[2][0] * i + change_un_Mat[2][1] * j
                            + change_un_Mat[2][2]));

            if (local_x >= 0 && local_y >= 0 && local_y < USED_ROW && local_x < USED_COL)
            {
                PerImg_ip[j][i] = &PER_IMG[local_y][local_x];
            }
            else
            {
                PerImg_ip[j][i] = &BlackColor;
            }
        }
    }
}

/* 偏移表初始化：存 y*188 + x，越界写 0（指向首像素） */
#elif (PERSPECTIVE_MODE == PERSPECTIVE_MODE_OFFSET)

void ImagePerspective_Init(void)
{
    for (int i = 0; i < RESULT_COL; i++)
    {
        for (int j = 0; j < RESULT_ROW; j++)
        {
            int local_x = (int)((change_un_Mat[0][0] * i
                    + change_un_Mat[0][1] * j + change_un_Mat[0][2])
                    / (change_un_Mat[2][0] * i + change_un_Mat[2][1] * j
                            + change_un_Mat[2][2]));
            int local_y = (int)((change_un_Mat[1][0] * i
                    + change_un_Mat[1][1] * j + change_un_Mat[1][2])
                    / (change_un_Mat[2][0] * i + change_un_Mat[2][1] * j
                            + change_un_Mat[2][2]));

            if (local_x >= 0 && local_y >= 0 && local_y < USED_ROW && local_x < USED_COL)
            {
                PerImg_ofs[j][i] = (int16)(local_y * USED_COL + local_x);
            }
            else
            {
                PerImg_ofs[j][i] = 0;
            }
        }
    }
}

/* U8 表初始化：每行先算一次 y（与列无关），再逐列算 x；整行越界就标 rvalid = 0 */
#else

void ImagePerspective_Init(void)
{
    for (int j = 0; j < RESULT_ROW; j++)
    {
        double denominator = change_un_Mat[2][1] * j + change_un_Mat[2][2];
        int    row_valid   = 0;
        int    local_y     = 0;

        if (denominator == 0.0)
        {
            PerImg_rowbase[j] = &mt9v03x_image[0][0];
            PerImg_rvalid[j]  = 0;
            for (int i = 0; i < RESULT_COL; i++) { PerImg_xcol[j][i] = 0; }
            continue;
        }

        local_y = (int)((change_un_Mat[1][1] * j + change_un_Mat[1][2]) / denominator);

        if (local_y < 0 || local_y >= USED_ROW)
        {
            PerImg_rowbase[j] = &mt9v03x_image[0][0];
            PerImg_rvalid[j]  = 0;
            for (int i = 0; i < RESULT_COL; i++) { PerImg_xcol[j][i] = 0; }
            continue;
        }

        PerImg_rowbase[j] = &mt9v03x_image[local_y][0];     /* y*188 提前算好 */

        for (int i = 0; i < RESULT_COL; i++)
        {
            int local_x = (int)((change_un_Mat[0][0] * i
                    + change_un_Mat[0][1] * j + change_un_Mat[0][2]) / denominator);

            if (local_x >= 0 && local_x < USED_COL)
            {
                PerImg_xcol[j][i] = (uint8)local_x;
                row_valid = 1;
            }
            else
            {
                PerImg_xcol[j][i] = 0;                      /* 行内越界，钳到行首 */
            }
        }

        PerImg_rvalid[j] = (uint8)row_valid;
    }
}

#endif


/* 全图遍历求和。三种方案都把行一级的工作提到内层循环外，口径一致 */
uint32 perspective_sum(void)
{
    uint32 sum = 0;

/* U8：每行取一次行基址和 x 数组首地址，整行无效则跳过 */
#if (PERSPECTIVE_MODE == PERSPECTIVE_MODE_U8)

    for (uint16 row = 0; row < RESULT_ROW; row++)
    {
        const uint8 *line = PerImg_rowbase[row];
        const uint8 *xc   = PerImg_xcol[row];

        if (0 == PerImg_rvalid[row])
        {
            continue;
        }
        for (uint16 col = 0; col < RESULT_COL; col++)
        {
            sum += line[xc[col]];
        }
    }

/* 偏移表：扁平一维，没有行一级工作 */
#elif (PERSPECTIVE_MODE == PERSPECTIVE_MODE_OFFSET)

    const int16   *table = &PerImg_ofs[0][0];
    const uint8_t *base  = (const uint8_t *)&mt9v03x_image[0][0];
    const uint32   count = (uint32)RESULT_ROW * RESULT_COL;

    for (uint32 i = 0; i < count; i++)
    {
        sum += base[table[i]];
    }

/* 指针表：每行取一次行首地址，内层直接解引用 */
#else

    for (uint16 row = 0; row < RESULT_ROW; row++)
    {
        uint8_t *const *line = PerImg_ip[row];

        for (uint16 col = 0; col < RESULT_COL; col++)
        {
            sum += *line[col];
        }
    }

#endif

    return sum;
}
