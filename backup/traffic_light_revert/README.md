# 红绿灯公共接口归档说明

本目录保存红绿灯公共输出移除前的相关文件，用于后续接口恢复和版本差异比对。

正式交付接口包含限速牌、禁令 / 停车牌和车牌结果。红绿灯检测模型、YOLOX 后处理及业务配置仍保留在主工程中，红绿灯公共类型和结果转换路径未纳入该交付接口。

## 归档内容

| 文件 | 归档内容 |
|------|----------|
| [include/alg_types.h](include/alg_types.h) | 红绿灯类型、颜色枚举及结果数组定义 |
| [include/alg_interface.h](include/alg_interface.h) | 对应版本的公共函数声明 |
| [src/core/object.cpp](src/core/object.cpp) | 红绿灯结果转换 |
| [src/interface/alg_interface.cpp](src/interface/alg_interface.cpp) | 结果数组释放逻辑 |
| [test/test_runner.cpp](test/test_runner.cpp) | 红绿灯结果打印和绘制 |

归档接口包含 `AlgTrafficLight`、`AlgTrafficLightColor` 以及 `AlgResult.traffic_lights[]`，不作为正式交付头文件使用。

## 恢复流程

1. 在独立开发分支中比对归档文件与主工程，确认归档后新增的接口和处理逻辑。
2. 合并红绿灯类型、结果转换、内存释放及测试程序的相关代码，保留主工程中的后续变更。
3. 使用对应后端的 `traffic_light.json` 和 `all.json` 验证单业务及组合业务输出。
4. 检查公共结构体布局、结果释放和调用方兼容性，重新编译 SDK 及应用程序。
5. 同步更新接口文档和版本记录后交付。

归档文件与主工程可能存在版本差异，恢复时按功能合并，避免整文件覆盖导致后续功能丢失。公共结构体发生变化后，头文件、动态库和应用程序应配套发布。
