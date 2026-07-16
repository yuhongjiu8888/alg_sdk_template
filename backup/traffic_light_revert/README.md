# 红绿灯输出还原备份

本目录保存 **屏蔽红绿灯输出之前** 的原始文件（取自屏蔽改动前的 git HEAD）。
当前工程暂不需要红绿灯，已在源码中直接删除红绿灯相关的对外类型与输出路径，
**没有用宏开关**（对外头文件保持干净，便于交付第三方）。

后续需要重新融合红绿灯能力时，直接用本目录文件覆盖回去即可：

```sh
cd <repo_root>
cp backup/traffic_light_revert/include/alg_types.h            include/alg_types.h
cp backup/traffic_light_revert/include/alg_interface.h        include/alg_interface.h
cp backup/traffic_light_revert/src/core/object.cpp            src/core/object.cpp
cp backup/traffic_light_revert/src/interface/alg_interface.cpp src/interface/alg_interface.cpp
cp backup/traffic_light_revert/test/test_runner.cpp           test/test_runner.cpp
```

覆盖后重新编译即可恢复 `AlgResult.traffic_lights[]` / `AlgTrafficLight` / `AlgTrafficLightColor`
以及 test_runner 的红绿灯绘制/打印。注意红绿灯的检测/后处理逻辑（yolox_det 的
`traffic_light` 分支、resources 里的 traffic_light.json / all.json）本次并未改动。
