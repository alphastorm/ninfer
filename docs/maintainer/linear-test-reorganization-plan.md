# Linear 测试的后续整理

当前保留 22 个入口：Linear 9、LinearAdd 6、LinearPair 1、LinearSwiGLU 4，以及
TopK、动态卷积融合各 1 个。共享机制和命令见 [Tests](../../tests/README.md)，数学与
资格要求见 [Op development](op-development.md)。本文只记录尚未完成的整理需求。

## 当前基础

`tests/ops/linear_common.h` 共享尺寸检查、明确的七点/均匀采样、payload 保持检查与
0/1/77 入口状态；各 Op 保留自己的完整 FP64 oracle、fixture recipe 和 criterion。
BF16 新旧几何已经共用 Linear runner，`--shape` 覆盖全部 BF16 几何；FP8 metadata
拒绝经公开入口验证。FP8/NVFP4 Add 已接入 Add runner，Graph 阶段显式声明并逐阶段
验证；Pair 的 parent/row views 由 case 声明。SwiGLU 的 skip 与重复 admission 已整理，
negative-gate 仍保留独立严格性质检查。TopK 使用 CXX oracle 编译选项，TopK 与动态卷积
各自拥有目录和 manifest。

## 剩余需求

- **统一 case 入口。** 当前仍由各格式入口构建调用表，只有 Q8/BF16 Linear 提供
  `--shape`。后续可让各 Op 的格式/profile case 交给同一 main/runner，统一
  `--help`、`--list`、`--shape N K`、`--case CASE_ID`；列举和筛选在 CUDA 初始化前
  完成，空匹配和非法输入明确失败。是否拆分现有聚合 executable 由维护需要决定。
- **采样与执行描述。** case 显式拥有 seed、recipe、policy、公开重载、采样和 replay
  计划；动态展开的数组由 suite 持有，span 只作借用。稳定 ID 区分同一 T 的不同输入、
  重载与阶段。runner 不维护生产支持白名单或私有 selector。
- **已确认的验证缺口。** Pair 两类 parent、TopK 和动态卷积仍需各自代表性的
  changed-input Graph；FP8/NVFP4 Add 当前仍只对数学采样位置检查有限值。新增检查
  单独说明保护的风险和运行成本，不因整理扩大为全组合样本。
- **公开重载与资源合同。** 整理 convenience/policy 两种入口的数值证据，保留
  permissive policy 下 A16 场景；核对 NVFP4 SwiGLU convenience 的 T=16/17 边界。
  Pair workspace 的零容量和非法区间、其他套件的 exact-peak 断言按公开合同核实。
- **共享辅助的后续收口。** `input_projection_test_common.h` 仍服务 Attention/GDN
  projection、snapshot/record、Replay fold 和动态卷积。若再迁移其中机制，更新全部
  真实调用者，保留数学 oracle 与同一物理 block 的指定 snapshot 精确 reference。
  不为统一目录重新复制大型 BF16 权重或引入生产 codec 作为 oracle。

## 迁移约束

保留实际几何、T/W/B、seed、输入公式、幅度、codes/scales/divisors、重载、采样集合
和已有 Graph 阶段；大矩阵继续使用既有采样及分块扫描。完整 K 的 naive FP64 累加
及各融合公式独立于生产实现，不加入私有 staging、激活量化或额外 BF16 舍入。

数值标准由 Op/profile 命名管理。SwiGLU negative-gate 的精确表示输入仍使用
`{relative_l2=1/256, gross_absolute=0, gross_relative=2/256}`，不能以一般 A8/A4
阈值替代。修改数据集、采样、比较规则或数学性质必须作为明确变更处理。

Graph 的每个声明阶段按实际输入验证，地址稳定且 residual 正确复位；guard、payload
保持和 workspace 检查保护各自真实边界。共用实现单次编译，oracle 所属 target 应用
`ninfer_op_oracle_options()`。验证使用原 suite 和实际受影响调用者；按 family/format
观察执行成本，不把测试整理变成 kernel 调优。完成这些需求后将使用说明归入 Tests，
删除本文。
