/**
 * @file aipp_scale.h
 * @brief 动态 AIPP 缩放的执行方（进程级运行时开关，模仿 alg::log::set_level 的形态）。
 *
 * 只有 Hi3519DV500 的后端会读它：该芯片的 AIPP 参与缩放时有固定 ~128ms 开销，
 * 需要调用方或 SDK 自己先把整帧缩到 aipp_output_size。
 */

#ifndef ALG_CORE_INFER_AIPP_SCALE_H
#define ALG_CORE_INFER_AIPP_SCALE_H

namespace alg {
namespace aipp_scale {

enum class Mode {
    kAuto,     /* 源尺寸不等于 aipp_output_size 时，由 SDK 预缩放 */
    kExternal, /* 缩放由调用方完成；源尺寸不符直接报错 */
};

/** 读取当前模式（运行期可能被 AlgSetAippScaleMode 改写）。 */
Mode mode();

/** 设置模式。由 AlgSetAippScaleMode 调用。 */
void set_mode(Mode mode);

}  // namespace aipp_scale
}  // namespace alg

#endif  // ALG_CORE_INFER_AIPP_SCALE_H
