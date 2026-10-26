# 逆透视查表法：把 88 KB 压到 22 KB

CH32V307，120 MHz，128 KB SRAM，无 D-Cache。图像 `mt9v03x_image[120][188]`，22560 像素，连续内存。

内存对比

![内存差距.png](_pic/%E5%86%85%E5%AD%98%E5%B7%AE%E8%B7%9D.png)

像素读取速度对比

![速度数据.png](_pic/%E9%80%9F%E5%BA%A6%E6%95%B0%E6%8D%AE.png)

逆透视表好用的关键在于"取像素"的动作要足够快，同时别把 SRAM 吃光。
以下三个方案，内存从 88 KB（原版） 砍到最小 22 KB。

| 模式     | 表大小     | RAM 占用            | -O0 耗时     | -O2 耗时     | -O0 内层指令 | -O2 内层指令 |
| ------ | ------- | ----------------- | ---------- | ---------- | -------- | -------- |
| 指针表    | 90240 B | 116296 B (89.43%) | 4728503 ns | 1323442 ns | 16       | 5        |
| 偏移表    | 45120 B | 71172 B (54.73%)  | 5452613 ns | 1504377 ns | 19       | 6        |
| U8 压缩表 | 23160 B | 49212 B (37.84%)  | 4358247 ns | 1449291 ns | 18       | 6        |

注意，这套数据的前提是**整幅图按行顺序遍历**。三个模式的耗时都是用 `perspective_sum()` 从第一行扫到最后一行测出来的，表是顺着读的，取像素也基本顺着走。真实工程里巡线、识别只挑需要的格子，属于随机存取，**那种访问方式下 U8 的"更快"不成立**，理由见后文。下面所有涉及"U8 最快"的地方都得带着这个前提看。

同一份代码在 CYT4BB7（Cortex-M7，250 MHz，IAR）上跑的时候，三个模式的排名随优化等级变（神秘编译器设置）。

## 仓库内容

| 文件                                | 内容                                             |
| --------------------------------- | ---------------------------------------------- |
| `Perspective.c` / `Perspective.h` | 逆透视本体，三种方案编译期用 `PERSPECTIVE_MODE` 切换           |
| `timing.h`                        | 计时器 + 全图遍历计时，CH32 用 SysTick，4BB7 用 DWT->CYCCNT |

`Perspective.c` / `Perspective.h` 两边通用，只有一行头文件包含不同：CH32 是 `main.h`，4BB7 是 `main_0.h`。

---

## 一、数学原理

逆透视是一次二维射影变换。给定单应矩阵 H，把目标图坐标 (i, j) 映射回原图的 (x, y)：

```
[x']     [h00 h01 h02]   [i]
[y']  =  [h10 h11 h12] · [j]
[w ]     [h20 h21 h22]   [1]
```

齐次坐标转回欧氏坐标要除以 w：

```
x = (h00·i + h01·j + h02) / (h20·i + h21·j + h22)
y = (h10·i + h11·j + h12) / (h20·i + h21·j + h22)
```

本工程标定出来的矩阵：

```c
double change_un_Mat[3][3] = {
    { 0.224880, -0.268106, 12.848148},
    {-0.000000,  0.024928,  5.810294},
    {-0.000000, -0.002784,  0.350978}
};
```

每个像素要算 6 次乘法、6 次加法、1 次除法，全是 double。22560 个像素跑一遍，光浮点除法的开销就没法看。所以实际做法是**离线算好映射关系存成表，运行时只查表**——把浮点运算从每帧一次挪到上电一次。

---

## 二、原版：指针表https://blog.csdn.net/wu58430/article/details/126317900

最直白的实现是存指针。初始化时逐格算好 (x, y)，把 `&mt9v03x_image[y][x]` 存进表里：

```c
uint8_t *PerImg_ip[RESULT_ROW][RESULT_COL];
```

取像素就一句解引用：

```c
sum += *PerImg_ip[row][col];
```

越界格填 `&BlackColor`，指向一个恒为 0 的静态字节。内层循环因此完全不需要判断，代价是越界信息被摊平到每一格里，运行时看不出哪一格是无效的。

表大小 `120 × 188 × 4 = 90240 B`，加上其他全局变量，RAM 吃到 116296 B，占 128 KB 的 89.43%。

问题很直接：一个字节的像素，用四个字节的指针去索引，浪费三倍。SRAM 只剩 11 KB，摄像头缓冲区、串口 FIFO、巡线算法的中间变量全挤在这儿，很容易爆。

---

## 三、偏移表

### 怎么想到的

指针的本质是 `基址 + 偏移`。而这里的基址是固定的——整幅图就是 `mt9v03x_image` 这一个连续数组的首地址，初始化之后永远不变。

既然基址不变，就没必要每格都存一遍。只存偏移，运行时再加回来：

```c
int16 PerImg_ofs[RESULT_ROW][RESULT_COL];
...
sum += *(base + PerImg_ofs[row][col]);
```

### 为什么可行

两个前提：

1. **图像是连续内存。** `mt9v03x_image[120][188]` 是一个完整的二维数组，`&mt9v03x_image[y][x]` 就是 `首地址 + y*188 + x`，没有行填充。
2. **偏移量装得下 int16。** 图像总共 22560 字节，最大偏移 22559，`int16` 上限 32767，富余。

于是 4 字节变 2 字节，表从 90240 B 降到 45120 B，正好一半。

### 性能损耗

|     | 指针表        | 偏移表        | 差距     |
| --- | ---------- | ---------- | ------ |
| -O0 | 4728503 ns | 5452613 ns | +15.3% |
| -O2 | 1323442 ns | 1504377 ns | +13.7% |

指令数差 18.75%（16→19）和 20%（5→6），实际时间差 15.3% 和 13.7%，比指令数差还小。因为循环里本来就有内存访问的停顿，多出来的 ALU 指令塞进了这些空隙里。

**省一半内存，换单个像素13~15% 的时间。** 这笔交易在 SRAM 紧张的时候是划算的。

---

## 四、U8 压缩表

### 回到原理：视角只在 XZ 平面变化

到这里内存还是 44 KB，还能再压。但再压需要新的信息，不能再靠"换个存储方式"。

回去看那个矩阵：

```c
{ 0.224880, -0.268106, 12.848148},
{-0.000000,  0.024928,  5.810294},
{-0.000000, -0.002784,  0.350978}
```

注意 **h10 = 0，h20 = 0**（写成 `-0.000000`，就是负零）。

相机装在车上，只有俯仰角（pitch）会变，没有偏航（yaw）、没有滚转（roll）。视角只在 XZ 平面内变化，地平线始终是水平的。

具体到系数上（H = K·[r1 r2 t]，K 是内参矩阵）：

- `h10 = f·r21`，正比于旋转矩阵的 r21 元素，对应滚转。
- `h20 = r31`，就是 r31，对应偏航。

两个都为 0，等价于"只有俯仰"。这不是推的，是验算过的：单独给 pitch 时 h10 = h20 = 0；一旦叠上 yaw，h20 立刻变成 −0.149；叠上 roll，h10 变成 +0.149。

把 h10 = h20 = 0 代回公式：

```
分母 D = h21·j + h22          ← 只跟 j 有关，跟 i 无关
y = (h11·j + h12) / D         ← 只跟 j 有关，跟 i 无关
x = (h00·i + h01·j + h02) / D ← 给定行之后，是 i 的仿射函数
```

**同一个 j（同一行），整行的 y 是同一个常数。**

这一条就是全部的关键。原版和偏移版每格都存了完整的 (x, y)，但实际上 188 个格子共享同一个 y，重复存了 188 遍。

顺手看 x 的范围：x 是**原图**的列坐标（不是目标图的列坐标），有效范围 0..187，`uint8` 上限 255，装得下。这里能压到 uint8 只是因为原图宽度是 188，换成 320 宽的摄像头就不够了。

于是表可以拆成：

```c
uint8  PerImg_xcol[120][188];   // 每格只存 x，22560 B
uint8 *PerImg_rowbase[120];     // 每行首像素地址，480 B
uint8  PerImg_rvalid[120];      // 该行是否有效，120 B
```

`rowbase[j]` 在初始化时就算好 `&mt9v03x_image[y][0]`，把 `y*188` 这个乘法提前到上电阶段，运行时内层循环不用再乘。

**PerImg_rvalid数组也可以优化掉，只是说这里没啥必要了**

合计 23160 B，22.6 KB。相对指针表省了 74 KB，相对偏移表省了 22 KB。

---

### 内层循环

`-O2` 下三个方案的内层循环：

```
指针表 -O2                    偏移表 -O2                   U8 -O2
lw    a4,0(a5)               lh    a4,0(a5)              lbu   a4,0(a5)
addi  a5,a5,4                addi  a5,a5,2               addi  a5,a5,1
lbu   a4,0(a4)               add   a4,a4,a2              add   a4,a4,a2
add   a0,a0,a4               lbu   a4,0(a4)              lbu   a4,0(a4)
bne   a3,a5,...              add   a0,a0,a4              add   a0,a0,a4
                             bne   a3,a5,...             bne   a5,a3,...

  5 条                          6 条                        6 条
```

U8 和偏移表都是 6 条，结构也几乎一样：一次查表拿地址分量，一次加法叠基址，一次取像素。区别只是表项从 int16 变成 uint8，`lh` 换成 `lbu`，`addi` 的步长从 2 变成 1。

`-O0` 下是 18 条：

```
U8 -O0（内层）
lhu   a5,-24(s0)   ; col
lw    a4,-32(s0)   ; x_col 行首
add   a5,a5,a4
lbu   a5,0(a5)     ; 取 x
mv    a4,a5
lw    a5,-28(s0)   ; rowbase[j]（已含 y*188）
add   a5,a5,a4
lbu   a5,0(a5)     ; 取像素
mv    a4,a5
lw    a5,-20(s0)   ; sum
add   a5,a5,a4
sw    a5,-20(s0)
lhu   a5,-24(s0)
addi  a5,a5,1
sh    a5,-24(s0)
lhu   a4,-24(s0)
li    a5,187
bgeu  a5,a4,...
```

行一级的工作（取 `rowbase`、判 `rvalid`、算 `x_col` 行首）全在循环外，每个像素只做"查 x → 加行基址 → 取像素 → 累加"。

### 为什么快

这里要分两种编译情况说，因为结论是反的。

**先看每像素成本。** U8 每取一个像素其实**更贵**：

| 模式      | 总时间        | 实取像素      | 每像素周期     |
| ------- | ---------- | --------- | --------- |
| 指针表 -O0 | 4728503 ns | 22560     | 25.15     |
| 偏移表 -O0 | 5452613 ns | 22560     | 29.00     |
| U8 -O0  | 4358247 ns | **19176** | **27.27** |
| 指针表 -O2 | 1323442 ns | 22560     | 7.04      |
| 偏移表 -O2 | 1504377 ns | 22560     | 8.00      |
| U8 -O2  | 1449291 ns | **19176** | **9.07**  |

U8 的每像素成本是 27.27，比指针表的 25.15 贵 8.4%。贵在内层 18 条 vs 16 条，多的是"x 查表后再加行基址"这一步——指针表一步到位，U8 要两段寻址。

**那为什么总时间反而少？** 因为它少算了 15% 的像素。19176 / 22560 = 0.85。

**跑分上U8 比原版快是因为顺序存取了** 这套矩阵正好有 18 行 y 出界，白捡 15%。换个标定让 120 行全部有效，`rvalid` 全为 1，这个优势就没了，U8 会稳定地比指针表慢 8.4%（-O0）或 28.8%（-O2）。

上面所有耗时都是**顺着扫整幅图**测出来的：从第 0 行扫到第 119 行，每行从第 0 列扫到第 187 列。表是顺序读的，取到的像素也大致顺着内存往前走。

真实的视觉算法不是这么用的。巡线是隔几行取几个点，识别是在某个区域内找特征，两者都属于**随机存取**——取哪一格事先不知道，也谈不上顺序。这时候三个模式的代价结构完全不一样：

**要是“扫线法”的话可以考虑U8节省时间**

真实工程基本是后者。**这就是为什么分档建议里 U8 的定位是"SRAM 紧张时的选择"，而不是"更快的选择"。**

### 理论上限

按"取满 22560 格"同口径外推（× 22560/19176 = 1.17647）：

| 模式     | 实测（102 行有效） | 理论上限（120 行全有效） |
| ------ | ----------- | -------------- |
| U8 -O0 | 4358247 ns  | 5127349 ns     |
| U8 -O2 | 1449291 ns  | 1705048 ns     |

这两个数代表矩阵特性失效时的耗时，代码里注释成 `理论上限` 就是给这个用的。

上限 5127349 ns 仍然快过偏移表的 5452613 ns，所以即使一行都跳不掉，U8 在 `-O0` 下也只是输给指针表，还能赢偏移表。这个排序在两种矩阵下都成立。

---

## 六、选型

| 场景                              | 选择                   |
| ------------------------------- | -------------------- |
| SRAM 紧张，能接受 `h10 = h20 = 0` 的前提 | U8                   |
| 需要 `-O2` 下最快的方案                 | 指针表 + O2（1323442 ns） |
| 矩阵可能重新标定，h10/h20 不为 0           | 指针表或偏移表              |
| 按坐标随机取点（巡线、识别）                  | 指针表（U8 在这里没有速度优势）    |

四条硬性提醒：

1. **U8 依赖 `h10 = h20 = 0`。** 重新标定后这两个值不为 0，U8 会算错，必须换回指针表或偏移表。
2. **表里的"U8 最快"只对顺序扫全图成立。** `-O0` 下 U8 快、`-O2` 下指针表快，这个排名都建立在整幅图依行遍历的前提上（理由见五、《这组数的口径：顺序存取》）。真实工程是随机取点，那个前提下 U8 的行一级开销提不到循环外，取一个点要连做三次查表，速度上打不过指针表。
3. **默认 `-O0` 时选 U8，前提是跑的是顺序遍历。** 工程默认 `-O0`，顺序遍历时 U8 最快，这个结论没变；但它不是真实算法的工作方式，省内存才是它稳妥的理由。
4. **不要用手写汇编。** 全部代码是标准 C，汇编由 GCC 生成。

### CYT4BB7 上重跑

同一份 C、同一套标定参数，只换芯片和编译器（IAR 9.40.1，250 MHz）。三个数依次是高 / 中 / 低优化等级：

| 模式  | 高         | 中          | 低          |
| --- | --------- | ---------- | ---------- |
| 指针表 | 742700 ns | 1605200 ns | 1052400 ns |
| 偏移表 | 743100 ns | 1920800 ns | 954000 ns  |
| U8  | 844400 ns | 1594300 ns | 1063700 ns |

**中等优化是最慢的，三个模式全都如此。** 高 < 低 < 中，这个顺序不寻常，正常应该高 ≤ 中 ≤ 低。不管它是什么原因，数据就是这样。

高等级下指针表和偏移表几乎打平（742700 vs 743100，差 0.05%），U8 落后 13.7%。CH32 上指针表在 `-O2` 下是明显最快的，4BB7 上没有这个差距。

排名随等级变：

| 等级  | 第一         | 第二          | 第三          |
| --- | ---------- | ----------- | ----------- |
| 高   | 指针表 742700 | 偏移表 743100  | U8 844400   |
| 中   | U8 1594300 | 指针表 1605200 | 偏移表 1920800 |
| 低   | 偏移表 954000 | 指针表 1052400 | U8 1063700  |

也就是说在 4BB7 上没有一个方案在所有等级下都赢，选型得先把优化等级钉住。

折成周期看，4BB7 每个像素反而更费：

| 模式  | CH32 -O2   | CYT4BB7 高  | CYT4BB7 低  |
| --- | ---------- | ---------- | ---------- |
| 指针表 | 158813 cyc | 185675 cyc | 263100 cyc |
| 偏移表 | 180525 cyc | 185775 cyc | 238500 cyc |
| U8  | 173915 cyc | 211100 cyc | 265925 cyc |

CH32 是 RV32IMAC，单发射、没 Cache、没分支预测；4BB7 是 M7，理论上一堆东西都更强。结果 U8 在 CH32 的 `-O2` 下是 173915 周期，4BB7 最好的等级也要 211100 周期，多 21%。原因没查，可能是循环结构不一样（附录 C 的反汇编能看出 4BB7 那边行循环多了一些搬数据的指令），也可能是别处。这里只记数，不下结论。

最后再点一次口径：**上面 4BB7 的数也是顺序遍历。** 它们回答的是"顺着一帧扫一遍要多久"，不是"巡检线上那几十个点要多久"。

---

## 附录 A：核心代码

完整实现见仓库里的 `Perspective.c` 和 `Perspective.h`，三种方案靠 `PERSPECTIVE_MODE` 编译期切换，只有被选中的那份会分配内存。下面只留骨架。

### A.1 表定义

```c
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
```

### A.2 U8 压缩表初始化

另外两种初始化都是逐格套单应公式，这里只放 U8 的，因为它把 y 提到了行一级。

```c
void ImagePerspective_Init(void)
{
    for (int j = 0; j < RESULT_ROW; j++)
    {
        double denominator = change_un_Mat[2][1] * j + change_un_Mat[2][2];  /* h20 = 0，与列无关 */
        int    row_valid   = 0;
        int    local_y     = 0;

        if (denominator == 0.0)                                             /* 防御 */
        {
            PerImg_rowbase[j] = &mt9v03x_image[0][0];
            PerImg_rvalid[j]  = 0;
            for (int i = 0; i < RESULT_COL; i++) { PerImg_xcol[j][i] = 0; }
            continue;
        }

        local_y = (int)((change_un_Mat[1][1] * j + change_un_Mat[1][2]) / denominator);

        if (local_y < 0 || local_y >= USED_ROW)                             /* 整行无效 */
        {
            PerImg_rowbase[j] = &mt9v03x_image[0][0];
            PerImg_rvalid[j]  = 0;
            for (int i = 0; i < RESULT_COL; i++) { PerImg_xcol[j][i] = 0; }
            continue;
        }

        PerImg_rowbase[j] = &mt9v03x_image[local_y][0];                     /* y*188 提前算好 */

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
                PerImg_xcol[j][i] = 0;                                      /* 行内越界，钳到行首 */
            }
        }

        PerImg_rvalid[j] = (uint8)row_valid;
    }
}
```

### A.3 取单像素宏

```c
/* 指针表 */
#define PERSPECTIVE_PIXEL(row, col)     (*(PerImg_ip[(row)][(col)]))

/* 偏移表 */
#define PERSPECTIVE_BASE                (&mt9v03x_image[0][0])
#define PERSPECTIVE_PIXEL(row, col)     ((uint32)(*(PERSPECTIVE_BASE + PerImg_ofs[(row)][(col)])))

/* U8 */
#define PERSPECTIVE_PIXEL(row, col)                                             \
    ((0 == PerImg_rvalid[(row)])                                                \
        ? 0u                                                                    \
        : (uint32)PerImg_rowbase[(row)][PerImg_xcol[(row)][(col)]])
```

### A.4 全图遍历

```c
uint32 perspective_sum(void)
{
    uint32 sum = 0;

#if (PERSPECTIVE_MODE == PERSPECTIVE_MODE_U8)

    for (uint16 row = 0; row < RESULT_ROW; row++)
    {
        const uint8 *line = PerImg_rowbase[row];
        const uint8 *xc   = PerImg_xcol[row];

        if (0 == PerImg_rvalid[row])
        {
            continue;                                                       /* 整行无效，跳过 */
        }
        for (uint16 col = 0; col < RESULT_COL; col++)
        {
            sum += line[xc[col]];
        }
    }

#elif (PERSPECTIVE_MODE == PERSPECTIVE_MODE_OFFSET)

    const int16   *table = &PerImg_ofs[0][0];
    const uint8_t *base  = (const uint8_t *)&mt9v03x_image[0][0];
    const uint32   count = (uint32)RESULT_ROW * RESULT_COL;

    for (uint32 i = 0; i < count; i++)
    {
        sum += base[table[i]];
    }

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
```

---

## 附录 B：CH32 反汇编

全部来自 `riscv-none-embed-objdump -d --no-show-raw-insn 逆透视测试.elf`。

B.1 到 B.3 是工程默认的 `-O0`，B.4 到 B.6 是把同一个函数单独用 `-O2` 编出来的结果（`__attribute__((optimize("O2")))`，仓库里为简洁起见没有保留这个变体，代码本身一模一样）。

### B.1 指针表 -O0（`perspective_sum`，内层 16 条）

```
9bfa  addi   sp,sp,-32
9bfc  sw     s0,28(sp)
9bfe  addi   s0,sp,32
9c00  sw     zero,-20(s0)      ; sum = 0
9c04  sh     zero,-22(s0)      ; row = 0
9c08  j      9c68
9c0a  lhu    a4,-22(s0)        ; row
9c0e  li     a5,752            ; 188*4
9c12  mul    a4,a4,a5
9c16  lui    a5,0x20006
9c1a  addi   a5,a5,-524        ; PerImg_ip
9c1e  add    a5,a5,a4
9c20  sw     a5,-28(s0)        ; 行基址（循环外，每行一次）
9c24  sh     zero,-24(s0)      ; col = 0
9c28  j      9c52
9c2a  lhu    a5,-24(s0)        ; ← 内层开始
9c2e  slli   a5,a5,0x2
9c30  lw     a4,-28(s0)
9c34  add    a5,a5,a4
9c36  lw     a5,0(a5)          ; 取指针
9c38  lbu    a5,0(a5)          ; 取像素
9c3c  mv     a4,a5
9c3e  lw     a5,-20(s0)
9c42  add    a5,a5,a4
9c44  sw     a5,-20(s0)
9c48  lhu    a5,-24(s0)
9c4c  addi   a5,a5,1
9c4e  sh     a5,-24(s0)
9c52  lhu    a4,-24(s0)
9c56  li     a5,187
9c5a  bgeu   a5,a4,9c2a        ; ← 内层结束
9c5e  lhu    a5,-22(s0)
9c62  addi   a5,a5,1
9c64  sh     a5,-22(s0)
9c68  lhu    a4,-22(s0)
9c6c  li     a5,119
9c70  bgeu   a5,a4,9c0a
9c74  lw     a5,-20(s0)
9c78  mv     a0,a5
9c7a  lw     s0,28(sp)
9c7c  addi   sp,sp,32
9c7e  ret
```

### B.2 偏移表 -O0（`perspective_sum`，内层 19 条）

```
9c0a  addi   sp,sp,-48
9c0c  sw     s0,44(sp)
9c0e  addi   s0,sp,48
9c10  sw     zero,-20(s0)      ; sum = 0
9c14  lui    a5,0x20006
9c18  addi   a5,a5,-528        ; PerImg_ofs
9c1c  sw     a5,-28(s0)        ; table
9c20  lui    a5,0x20000
9c24  addi   a5,a5,1376        ; mt9v03x_image
9c28  sw     a5,-32(s0)        ; base
9c2c  lui    a5,0x6
9c2e  addi   a5,a5,-2016       ; 22560
9c32  sw     a5,-36(s0)        ; count
9c36  sw     zero,-24(s0)      ; i = 0
9c3a  j      9c6e
9c3c  lw     a5,-24(s0)        ; ← 内层开始
9c40  slli   a5,a5,0x1
9c42  lw     a4,-28(s0)
9c46  add    a5,a5,a4
9c48  lh     a5,0(a5)          ; 取偏移
9c4c  mv     a4,a5
9c4e  lw     a5,-32(s0)        ; 取图像基址（-O0 的锅）
9c52  add    a5,a5,a4          ; ← 偏移寻址的代价
9c54  lbu    a5,0(a5)          ; 取像素
9c58  mv     a4,a5
9c5a  lw     a5,-20(s0)
9c5e  add    a5,a5,a4
9c60  sw     a5,-20(s0)
9c64  lw     a5,-24(s0)
9c68  addi   a5,a5,1
9c6a  sw     a5,-24(s0)
9c6e  lw     a4,-24(s0)
9c72  lw     a5,-36(s0)
9c76  bltu   a4,a5,9c3c        ; ← 内层结束
9c7a  lw     a5,-20(s0)
9c7e  mv     a0,a5
9c80  lw     s0,44(sp)
9c82  addi   sp,sp,48
9c84  ret
```

### B.3 U8 -O0（`perspective_sum`，内层 18 条）

```
9c10  addi   sp,sp,-32
9c12  sw     s0,28(sp)
9c14  addi   s0,sp,32
9c16  sw     zero,-20(s0)      ; sum = 0
9c1a  sh     zero,-22(s0)      ; row = 0
9c1e  j      9cb4
9c20  lhu    a4,-22(s0)        ; row
9c24  lui    a5,0x2000b
9c28  slli   a4,a4,0x2
9c2a  addi   a5,a5,1552        ; PerImg_rowbase
9c2e  add    a5,a5,a4
9c30  lw     a5,0(a5)          ; rowbase[row]
9c32  sw     a5,-28(s0)        ; line（每行一次）
9c36  lhu    a4,-22(s0)
9c3a  li     a5,188
9c3e  mul    a4,a4,a5
9c42  lui    a5,0x20006
9c46  addi   a5,a5,-528        ; PerImg_xcol
9c4a  add    a5,a5,a4
9c4c  sw     a5,-32(s0)        ; xc（每行一次）
9c50  lhu    a4,-22(s0)
9c54  lui    a5,0x2000b
9c58  addi   a5,a5,2032        ; PerImg_rvalid
9c5c  add    a5,a5,a4
9c5e  lbu    a5,0(a5)          ; rvalid[row]
9c62  beqz   a5,9ca8           ; 整行无效则跳过
9c64  sh     zero,-24(s0)      ; col = 0
9c68  j      9c9a
9c6a  lhu    a5,-24(s0)        ; ← 内层开始
9c6e  lw     a4,-32(s0)
9c72  add    a5,a5,a4
9c74  lbu    a5,0(a5)          ; 取 x
9c78  mv     a4,a5
9c7a  lw     a5,-28(s0)        ; rowbase[j]
9c7e  add    a5,a5,a4
9c80  lbu    a5,0(a5)          ; 取像素
9c84  mv     a4,a5
9c86  lw     a5,-20(s0)
9c8a  add    a5,a5,a4
9c8c  sw     a5,-20(s0)
9c90  lhu    a5,-24(s0)
9c94  addi   a5,a5,1
9c96  sh     a5,-24(s0)
9c9a  lhu    a4,-24(s0)
9c9e  li     a5,187
9ca2  bgeu   a5,a4,9c6a        ; ← 内层结束
9ca6  j      9caa
9ca8  nop
9caa  lhu    a5,-22(s0)
9cae  addi   a5,a5,1
9cb0  sh     a5,-22(s0)
9cb4  lhu    a4,-22(s0)
9cb8  li     a5,119
9cbc  bgeu   a5,a4,9c20
9cc0  lw     a5,-20(s0)
9cc4  mv     a0,a5
9cc6  lw     s0,28(sp)
9cc8  addi   sp,sp,32
9cca  ret
```

### B.4 指针表 -O2（`perspective_sum`，内层 5 条）

```
9bfa  lui    a2,0x20006
9bfe  lui    a5,0x16
9c00  addi   a2,a2,-524        ; PerImg_ip
9c04  addi   a5,a5,880         ; 752*120
9c08  addi   a3,a2,752         ; 第一行末尾
9c0c  li     a0,0              ; sum = 0
9c0e  add    a2,a2,a5          ; 表末尾
9c10  addi   a5,a3,-752        ; 行首
9c14  lw     a4,0(a5)          ; ← 内层开始，取指针
9c16  addi   a5,a5,4
9c18  lbu    a4,0(a4)          ; 取像素
9c1c  add    a0,a0,a4
9c1e  bne    a3,a5,9c14        ; ← 内层结束
9c22  addi   a3,a3,752
9c26  bne    a3,a2,9c10
9c2a  ret
```

### B.5 偏移表 -O2（`perspective_sum`，内层 6 条）

```
9c0a  lui    a5,0x20006
9c0e  lui    a3,0xb
9c10  addi   a5,a5,-528        ; PerImg_ofs
9c14  addi   a3,a3,64          ; 45120
9c18  lui    a2,0x20000
9c1c  add    a3,a3,a5          ; 表末尾
9c1e  li     a0,0              ; sum = 0
9c20  addi   a2,a2,1376        ; mt9v03x_image
9c24  lh     a4,0(a5)          ; ← 内层开始，取偏移
9c28  addi   a5,a5,2
9c2a  add    a4,a4,a2          ; + 图像基址
9c2c  lbu    a4,0(a4)          ; 取像素
9c30  add    a0,a0,a4
9c32  bne    a3,a5,9c24        ; ← 内层结束
9c36  ret
```

### B.6 U8 -O2（`perspective_sum`，内层 6 条）

```
9c10  lui    a1,0x2000b
9c14  addi   a1,a1,2032        ; PerImg_rvalid
9c18  lui    a3,0x20006
9c1c  lui    a6,0x2000b
9c20  addi   a3,a3,-340        ; PerImg_xcol - 188
9c24  addi   a6,a6,1552        ; PerImg_rowbase
9c28  addi   a7,a1,120         ; rvalid 末尾
9c2c  li     a0,0              ; sum = 0
9c2e  j      9c3c
9c30  addi   a1,a1,1
9c32  addi   a3,a3,188
9c36  addi   a6,a6,4
9c38  beq    a1,a7,9c68
9c3c  lbu    a5,0(a1)          ; rvalid[row]
9c40  beqz   a5,9c30           ; 整行无效则跳过
9c42  lw     a2,0(a6)          ; rowbase[row]（含 y*188）
9c46  addi   a5,a3,-188        ; xc = &xcol[row][0]
9c4a  lbu    a4,0(a5)          ; ← 内层开始，取 x
9c4e  addi   a5,a5,1
9c50  add    a4,a4,a2          ; + 行基址
9c52  lbu    a4,0(a4)          ; 取像素
9c56  add    a0,a0,a4
9c58  bne    a5,a3,9c4a        ; ← 内层结束
9c5c  addi   a1,a1,1
9c5e  addi   a3,a3,188
9c62  addi   a6,a6,4
9c64  bne    a1,a7,9c3c
9c68  ret
```

---

---

## 附录 C：CYT4BB7 反汇编（Cortex-M7）

CYT4BB7，编译器 IAR 9.40.1，取法：

```
ielfdumparm --code Debug_m7_0/Exe/cyt4bb7_cm_7_0.out
```

`perspective_sum` 在 High 下没有独立符号，被整个内联进 `perspective_test_run`，所以下面每小节只有一整个函数。

### C.1 IAR 三档优化等级的区别

IAR 这三档改的不只是优化力度，还包括编译器愿不愿意做某些事。

|         | None (`CCOptLevel = 0`) | Medium (`2`) | High (`3`)    |
| ------- | ----------------------- | ------------ | ------------- |
| 变量存放    | 基本都在栈上，每次读写都过内存         | 尽量放寄存器，跨语句保持 | 同 Medium，且更激进 |
| 函数内联    | 不做                      | 少量，小函数可能被内联  | 积极，小函数基本都内联   |
| 循环展开    | 不做                      | 不做           | 会做（这里展开了 4 次） |
| 循环不变量外提 | 不做                      | 会做           | 会做            |
| 公共子表达式  | 不做                      | 会做           | 会做            |
| 死代码消除   | 弱                       | 强            | 最强            |

下面三份反汇编都是 High，对应到这份代码上是：

- `perspective_sum` 被内联，符号消失，`perspective_test_run` 变成一个整体
- 内层循环展开 4 次，循环控制开销摊到 4 个像素上
- 行基址、表基址全部提前算好放寄存器
- 除法常量被换成乘法加移位（`UMULL` + `LSRS`），只有 `cycle * 1e9 / freq` 还走 `__aeabi_uldivmod`

### C.2 指针表 TABLE，优化等级 High

`CCOptLevel = 3`（IAR 的 High，命令行等价 `-Oh`）。`PERSPECTIVE_MODE = 0`。

```
0x1008'44b0  PUSH.W     {R4-R11, LR}
0x1008'44b4  MOVS       R4, R0
0x1008'44b6  BEQ.N      ??perspective_test_run_0
0x1008'44b8  LDR.N      R3, ??DataTable2_5 ; cpu_time_ready
0x1008'44ba  LDR.N      R5, ??DataTable2_6 ; 0xe000'1000
0x1008'44bc  LDRB       R0, [R3]
0x1008'44be  CBNZ       R0, ??perspective_test_run_1
0x1008'44c0  LDR.N      R0, ??DataTable2_7 ; VTOR + 0xf4
0x1008'44c2  LDR        R1, [R0]
0x1008'44c4  ORR.W      R1, R1, #16777216 ; 0x100'0000
0x1008'44c8  STR        R1, [R0]
0x1008'44ca  MOVS       R0, #0
0x1008'44cc  STR        R0, [R5, #0x4]
0x1008'44ce  LDR        R2, [R5]
0x1008'44d0  MOVS       R1, #1
0x1008'44d2  ORR.W      R2, R2, #1
0x1008'44d6  STR        R2, [R5]
0x1008'44d8  STRB       R1, [R3]
0x1008'44da  MOVS       R0, #0
0x1008'44dc  MOVS       R7, #0
0x1008'44de  STR        R0, [R5, #0x4]
0x1008'44e0  LDR.N      R6, ??DataTable2_4 ; PerImg_ip
0x1008'44e2  MOVS       R2, #120 ; 0x78
0x1008'44e4  MOV        R1, R6
0x1008'44e6  MOVS       R0, #47 ; 0x2f
0x1008'44e8  LDR.W      R12, [R1], #0x4
0x1008'44ec  SUBS       R0, R0, #1
0x1008'44ee  LDR.W      R8, [R1], #0x4
0x1008'44f2  LDR.W      R10, [R1], #0x4
0x1008'44f6  LDRB.W     LR, [R12]
0x1008'44fa  LDR.W      R12, [R1], #0x4
0x1008'44fe  LDRB.W     R9, [R8]
0x1008'4502  LDRB.W     R11, [R10]
0x1008'4506  ADD        R7, R7, LR
0x1008'4508  LDRB.W     LR, [R12]
0x1008'450c  ADD        R7, R7, R9
0x1008'450e  ADD        R7, R7, R11
0x1008'4510  ADD        R7, R7, LR
0x1008'4512  BNE.N      ??perspective_test_run_3
0x1008'4514  SUBS       R2, R2, #1
0x1008'4516  ADD.W      R6, R6, #752 ; 0x2f0
0x1008'451a  BNE.N      ??perspective_test_run_2
0x1008'451c  LDR        R5, [R5, #0x4]
0x1008'451e  STR        R7, [R3, #0x4]
0x1008'4520  LDR.N      R0, ??DataTable2_8 ; SystemCoreClock
0x1008'4522  LDR        R2, [R0]
0x1008'4524  CBNZ       R2, ??perspective_test_run_4
0x1008'4526  LDR.N      R2, ??DataTable2_9 ; 0xee6'b280
0x1008'4528  LDR.N      R0, ??DataTable2_10 ; 0x3b9a'ca00
0x1008'452a  MOVS       R3, #0
0x1008'452c  UMULL      R0, R1, R0, R5
0x1008'4530  STR        R5, [R4]
0x1008'4532  LDR.W      R8, ??DataTable2_11 ; 0x8312'6e98
0x1008'4536  BL         __aeabi_uldivmod ; 0x1008'45d4
0x1008'453a  STR        R0, [R4, #0x4]
0x1008'453c  MOV        R6, R0
0x1008'453e  LDR        R0, [R4, #0x4]
0x1008'4540  LDR.N      R2, ??DataTable2_12 ; 0xb9ea'f063
0x1008'4542  UMULL      R8, R7, R6, R8
0x1008'4546  LDR.N      R6, ??DataTable2_12 ; 0xb9ea'f063
0x1008'4548  UMULL      R6, R3, R5, R6
0x1008'454c  UMULL      R2, R1, R0, R2
0x1008'4550  LSRS       R7, R7, #9
0x1008'4552  LSRS       R3, R3, #14
0x1008'4554  STR        R7, [R4, #0x8]
0x1008'4556  LSRS       R1, R1, #14
0x1008'4558  STR        R3, [R4, #0x10]
0x1008'455a  STR        R1, [R4, #0xc]
0x1008'455c  POP.W      {R4-R11, PC}
```

### C.3 偏移表 OFFSET，优化等级 High

`CCOptLevel = 3`（IAR 的 High，命令行等价 `-Oh`）。`PERSPECTIVE_MODE = 1`。

```
0x1008'44e0  PUSH.W     {R4-R10, LR}
0x1008'44e4  MOVS       R4, R0
0x1008'44e6  BEQ.N      ??perspective_test_run_0
0x1008'44e8  LDR.N      R1, ??DataTable2_4 ; cpu_time_ready
0x1008'44ea  LDR.N      R2, ??DataTable2_5 ; 0xe000'1000
0x1008'44ec  LDRB       R0, [R1]
0x1008'44ee  CBNZ       R0, ??perspective_test_run_1
0x1008'44f0  LDR.N      R0, ??DataTable2_6 ; VTOR + 0xf4
0x1008'44f2  LDR        R3, [R0]
0x1008'44f4  ORR.W      R3, R3, #16777216 ; 0x100'0000
0x1008'44f8  STR        R3, [R0]
0x1008'44fa  MOVS       R0, #0
0x1008'44fc  STR        R0, [R2, #0x4]
0x1008'44fe  LDR        R5, [R2]
0x1008'4500  MOVS       R3, #1
0x1008'4502  ORR.W      R5, R5, #1
0x1008'4506  STR        R5, [R2]
0x1008'4508  STRB       R3, [R1]
0x1008'450a  MOVS       R0, #0
0x1008'450c  MOVS       R6, #0
0x1008'450e  STR        R0, [R2, #0x4]
0x1008'4510  LDR.N      R5, ??DataTable2_3 ; PerImg_ofs
0x1008'4512  MOVW       R0, #5640 ; 0x1608
0x1008'4516  LDR.N      R3, ??DataTable2_1 ; mt9v03x_image
0x1008'4518  LDRSH.W    R7, [R5], #0x2
0x1008'451c  SUBS       R0, R0, #1
0x1008'451e  LDRSH.W    LR, [R5], #0x2
0x1008'4522  LDRSH.W    R9, [R5], #0x2
0x1008'4526  LDRB.W     R12, [R3, R7]
0x1008'452a  LDRSH.W    R7, [R5], #0x2
0x1008'452e  LDRB.W     R8, [R3, LR]
0x1008'4532  LDRB.W     R10, [R3, R9]
0x1008'4536  ADD        R6, R6, R12
0x1008'4538  LDRB.W     R12, [R3, R7]
0x1008'453c  ADD        R6, R6, R8
0x1008'453e  ADD        R6, R6, R10
0x1008'4540  ADD        R6, R6, R12
0x1008'4542  BNE.N      ??perspective_test_run_2
0x1008'4544  LDR        R5, [R2, #0x4]
0x1008'4546  STR        R6, [R1, #0x4]
0x1008'4548  LDR.N      R0, ??DataTable2_7 ; SystemCoreClock
0x1008'454a  LDR        R2, [R0]
0x1008'454c  CBNZ       R2, ??perspective_test_run_3
0x1008'454e  LDR.N      R2, ??DataTable2_8 ; 0xee6'b280
0x1008'4550  LDR.N      R0, ??DataTable2_9 ; 0x3b9a'ca00
0x1008'4552  MOVS       R3, #0
0x1008'4554  UMULL      R0, R1, R0, R5
0x1008'4558  STR        R5, [R4]
0x1008'455a  LDR.W      R8, ??DataTable2_10 ; 0x8312'6e98
0x1008'455e  BL         __aeabi_uldivmod ; 0x1008'45f8
0x1008'4562  STR        R0, [R4, #0x4]
0x1008'4564  MOV        R6, R0
0x1008'4566  LDR        R0, [R4, #0x4]
0x1008'4568  LDR.N      R2, ??DataTable2_11 ; 0xb9ea'f063
0x1008'456a  UMULL      R8, R7, R6, R8
0x1008'456e  LDR.N      R6, ??DataTable2_11 ; 0xb9ea'f063
0x1008'4570  UMULL      R6, R3, R5, R6
0x1008'4574  UMULL      R2, R1, R0, R2
0x1008'4578  LSRS       R7, R7, #9
0x1008'457a  LSRS       R3, R3, #14
0x1008'457c  STR        R7, [R4, #0x8]
0x1008'457e  LSRS       R1, R1, #14
0x1008'4580  STR        R3, [R4, #0x10]
0x1008'4582  STR        R1, [R4, #0xc]
0x1008'4584  POP.W      {R4-R10, PC}
```

### C.4 U8 压缩表，优化等级 High

`CCOptLevel = 3`（IAR 的 High，命令行等价 `-Oh`）。`PERSPECTIVE_MODE = 2`。

```
0x1008'45e8  PUSH.W     {R4-R11, LR}
0x1008'45ec  MOVS       R4, R0
0x1008'45ee  BEQ.N      ??perspective_test_run_0
0x1008'45f0  LDR.W      R9, ??DataTable2 ; change_un_Mat
0x1008'45f4  LDR.N      R6, ??DataTable2_3 ; 0xe000'1000
0x1008'45f6  LDRB.W     R0, [R9, #0x48]
0x1008'45fa  CBNZ       R0, ??perspective_test_run_1
0x1008'45fc  LDR.N      R0, ??DataTable2_4 ; VTOR + 0xf4
0x1008'45fe  LDR        R1, [R0]
0x1008'4600  ORR.W      R1, R1, #16777216 ; 0x100'0000
0x1008'4604  STR        R1, [R0]
0x1008'4606  MOVS       R0, #0
0x1008'4608  STR        R0, [R6, #0x4]
0x1008'460a  LDR        R2, [R6]
0x1008'460c  MOVS       R1, #1
0x1008'460e  ORR.W      R2, R2, #1
0x1008'4612  STR        R2, [R6]
0x1008'4614  STRB.W     R1, [R9, #0x48]
0x1008'4618  MOVS       R0, #0
0x1008'461a  MOV        R8, R0
0x1008'461c  STR        R0, [R6, #0x4]
0x1008'461e  ADD.W      R7, R9, #556 ; 0x22c
0x1008'4622  ADD.W      R12, R9, #76 ; 0x4c
0x1008'4626  LDR.W      LR, ??DataTable2_1 ; PerImg_xcol
0x1008'462a  MOVS       R1, #120 ; 0x78
0x1008'462c  LDRB       R0, [R7]
0x1008'462e  LDR.W      R2, [R12]
0x1008'4632  CBZ        R0, ??perspective_test_run_3
0x1008'4634  MOV        R3, LR
0x1008'4636  MOVS       R0, #47 ; 0x2f
0x1008'4638  LDRB.W     R5, [R3], #0x1
0x1008'463c  SUBS       R0, R0, #1
0x1008'463e  LDRB.W     R11, [R3], #0x1
0x1008'4642  LDRB.W     R10, [R2, R5]
0x1008'4646  LDRB.W     R5, [R2, R11]
0x1008'464a  ADD        R8, R8, R10
0x1008'464c  ADD        R8, R8, R5
0x1008'464e  LDRB.W     R5, [R3], #0x1
0x1008'4652  LDRB       R5, [R2, R5]
0x1008'4654  ADD        R8, R8, R5
0x1008'4656  LDRB.W     R5, [R3], #0x1
0x1008'465a  LDRB.W     R10, [R2, R5]
0x1008'465e  ADD        R8, R8, R10
0x1008'4660  BNE.N      ??perspective_test_run_4
0x1008'4662  ADDS       R7, R7, #1
0x1008'4664  SUBS       R1, R1, #1
0x1008'4666  ADD.W      LR, LR, #188 ; 0xbc
0x1008'466a  ADD.W      R12, R12, #4
0x1008'466e  BNE.N      ??perspective_test_run_2
0x1008'4670  LDR        R6, [R6, #0x4]
0x1008'4672  STR.W      R8, [R9, #0x2a4]
0x1008'4676  LDR.N      R0, ??DataTable2_5 ; SystemCoreClock
0x1008'4678  LDR        R2, [R0]
0x1008'467a  CBNZ       R2, ??perspective_test_run_5
0x1008'467c  LDR.N      R2, ??DataTable2_6 ; 0xee6'b280
0x1008'467e  LDR.N      R0, ??DataTable2_7 ; 0x3b9a'ca00
0x1008'4680  MOVS       R3, #0
0x1008'4682  UMULL      R0, R1, R0, R6
0x1008'4686  STR        R6, [R4]
0x1008'4688  LDR.W      R8, ??DataTable2_8 ; 0x8312'6e98
0x1008'468c  BL         __aeabi_uldivmod ; 0x1008'4728
0x1008'4690  STR        R0, [R4, #0x4]
0x1008'4692  MOV        R9, R0
0x1008'4694  LDR        R0, [R4, #0x4]
0x1008'4696  LDR.N      R2, ??DataTable2_10 ; 0xb9ea'f063
0x1008'4698  UMULL      R8, R7, R9, R8
0x1008'469c  LDR.W      R9, ??DataTable2_10 ; 0xb9ea'f063
0x1008'46a0  UMULL      R9, R3, R6, R9
0x1008'46a4  UMULL      R2, R1, R0, R2
0x1008'46a8  LSRS       R7, R7, #9
0x1008'46aa  LSRS       R3, R3, #14
0x1008'46ac  STR        R7, [R4, #0x8]
0x1008'46ae  LSRS       R1, R1, #14
0x1008'46b0  STR        R3, [R4, #0x10]
0x1008'46b2  STR        R1, [R4, #0xc]
0x1008'46b4  POP.W      {R4-R11, PC}
```
