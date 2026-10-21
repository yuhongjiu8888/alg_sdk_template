#include <cstring>
#include <iostream>

/**
 * @brief 高性能 YUV420SP (NV12/NV21) 纯软跨步长下采样接口
 *
 * @param pSrc        输入 1080P 图像的 MMZ/普通内存虚拟地址指针
 * @param srcWidth    输入图像宽度 (如 1920)
 * @param srcHeight   输入图像高度 (如 1080)
 * @param pDst        输出模型尺寸图像的内存指针（需提前分配好空间）
 * @param dstWidth    目标图像宽度 (如 640)
 * @param dstHeight   目标图像高度 (如 360)
 * @param isNV21      如果是 NV21 传 true，如果是 NV12 传 false (仅影响 UV 顺序，不影响速度)
 */
inline void FastYuv420spResize(const unsigned char* pSrc,
                               int srcWidth,
                               int srcHeight,
                               unsigned char* pDst,
                               int dstWidth,
                               int dstHeight,
                               bool isNV21 = false) {
    // 1. 计算缩放步长 (Stride)
    float scaleX = (float)srcWidth / dstWidth;
    float scaleY = (float)srcHeight / dstHeight;

    // 2. 定位输入/输出的 Y 分量和 UV 分量起始地址
    const unsigned char* pSrcY = pSrc;
    const unsigned char* pSrcUV = pSrc + srcWidth * srcHeight;

    unsigned char* pDstY = pDst;
    unsigned char* pDstUV = pDst + dstWidth * dstHeight;

    // 3. 加速处理 Y 分量 (亮度通道)
    for (int y = 0; y < dstHeight; ++y) {
        // 计算当前目标行对应输入源图像的哪一行
        int srcY = (int)(y * scaleY);
        if (srcY >= srcHeight)
            srcY = srcHeight - 1;

        const unsigned char* pSrcYRow = pSrcY + srcY * srcWidth;
        unsigned char* pDstYRow = pDstY + y * dstWidth;

        for (int x = 0; x < dstWidth; ++x) {
            int srcX = (int)(x * scaleX);
            if (srcX >= srcWidth)
                srcX = srcWidth - 1;

            // 纯内存指针跳转赋值
            pDstYRow[x] = pSrcYRow[srcX];
        }
    }

    // 4. 加速处理 UV 分量 (色度通道：YUV420sp 格式中 UV 是交错存储的，且宽高只有 Y 的一半)
    int dstHeightUV = dstHeight / 2;
    int dstWidthUV = dstWidth / 2;
    int srcWidthUV = srcWidth / 2;

    for (int y = 0; y < dstHeightUV; ++y) {
        // UV 的行步长对应 Y 而言是两倍关系
        int srcY = (int)(y * scaleY) * 2;
        if (srcY >= srcHeight)
            srcY = srcHeight - 2;

        const unsigned char* pSrcUVRow = pSrcUV + (srcY / 2) * srcWidth;
        unsigned char* pDstUVRow = pDstUV + y * dstWidth;

        for (int x = 0; x < dstWidthUV; ++x) {
            int srcX = (int)(x * scaleX);
            // 钳位基准必须是源 UV 平面的宽度。用 dstWidthUV * (int)scaleX 在整数倍
            // 缩放时恰好相等，非整数倍时会提前钳死，把 UV 右半边抹成一根竖线。
            if (srcX >= srcWidthUV)
                srcX = srcWidthUV - 1;

            // YUV420sp 的 UV 是成对出现的 (U-V-U-V 或 V-U-V-U)
            // 每一个 UV 对占用 2 字节，所以寻址时乘 2
            pDstUVRow[x * 2] = pSrcUVRow[srcX * 2];          // U (或V)
            pDstUVRow[x * 2 + 1] = pSrcUVRow[srcX * 2 + 1];  // V (或U)
        }
    }
}

/**
 * @brief YUV420SP (NV12/NV21) 区域平均（box filter）下采样
 *
 * 与上面的最近邻版本相比：每个目标像素取源图对应矩形窗口的均值。
 * 最近邻在 3 倍下采样时每 9 个像素只保留 1 个，车牌这类细笔画会被抽稀掉，
 * 导致识别置信度掉到阈值以下；区域平均不会有这个问题。
 *
 * 参数含义与 FastYuv420spResize 一致（isNV21 只影响 UV 顺序）。
 * 内层用整数累加，无浮点运算；3 倍下采样时每目标像素 3 次加法（UV 为 9 次）。
 */
inline void FastYuv420spResizeBox(const unsigned char* pSrc,
                                  int srcWidth,
                                  int srcHeight,
                                  unsigned char* pDst,
                                  int dstWidth,
                                  int dstHeight,
                                  bool isNV21 = false) {
    (void)isNV21;
    const float scaleX = (float)srcWidth / dstWidth;
    const float scaleY = (float)srcHeight / dstHeight;

    const unsigned char* pSrcY = pSrc;
    const unsigned char* pSrcUV = pSrc + srcWidth * srcHeight;
    unsigned char* pDstY = pDst;
    unsigned char* pDstUV = pDst + dstWidth * dstHeight;

    // Y 分量：目标像素 (y,x) 覆盖源窗口 [x0,x1) x [y0,y1)
    for (int y = 0; y < dstHeight; ++y) {
        int y0 = (int)(y * scaleY);
        int y1 = (int)((y + 1) * scaleY);
        if (y1 <= y0)
            y1 = y0 + 1;
        if (y1 > srcHeight)
            y1 = srcHeight;
        if (y0 >= srcHeight)
            y0 = srcHeight - 1;

        unsigned char* pDstYRow = pDstY + (size_t)y * dstWidth;
        for (int x = 0; x < dstWidth; ++x) {
            int x0 = (int)(x * scaleX);
            int x1 = (int)((x + 1) * scaleX);
            if (x1 <= x0)
                x1 = x0 + 1;
            if (x1 > srcWidth)
                x1 = srcWidth;
            if (x0 >= srcWidth)
                x0 = srcWidth - 1;

            int sum = 0, cnt = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const unsigned char* pRow = pSrcY + (size_t)sy * srcWidth;
                for (int sx = x0; sx < x1; ++sx) {
                    sum += pRow[sx];
                    ++cnt;
                }
            }
            pDstYRow[x] = (unsigned char)(sum / (cnt > 0 ? cnt : 1));
        }
    }

    // UV 分量：以“UV 对”为单位，横向仍是 scaleX、纵向仍是 scaleY
    const int dstHeightUV = dstHeight / 2;
    const int dstWidthUV = dstWidth / 2;
    const int srcHeightUV = srcHeight / 2;
    const int srcWidthUV = srcWidth / 2;

    for (int y = 0; y < dstHeightUV; ++y) {
        int y0 = (int)(y * scaleY);
        int y1 = (int)((y + 1) * scaleY);
        if (y1 <= y0)
            y1 = y0 + 1;
        if (y1 > srcHeightUV)
            y1 = srcHeightUV;
        if (y0 >= srcHeightUV)
            y0 = srcHeightUV - 1;

        unsigned char* pDstUVRow = pDstUV + (size_t)y * dstWidth;
        for (int x = 0; x < dstWidthUV; ++x) {
            int x0 = (int)(x * scaleX);
            int x1 = (int)((x + 1) * scaleX);
            if (x1 <= x0)
                x1 = x0 + 1;
            if (x1 > srcWidthUV)
                x1 = srcWidthUV;
            if (x0 >= srcWidthUV)
                x0 = srcWidthUV - 1;

            int sumU = 0, sumV = 0, cnt = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const unsigned char* pRow = pSrcUV + (size_t)sy * srcWidth;
                for (int sx = x0; sx < x1; ++sx) {
                    sumU += pRow[sx * 2];
                    sumV += pRow[sx * 2 + 1];
                    ++cnt;
                }
            }
            if (cnt <= 0)
                cnt = 1;
            pDstUVRow[x * 2] = (unsigned char)(sumU / cnt);
            pDstUVRow[x * 2 + 1] = (unsigned char)(sumV / cnt);
        }
    }
}
