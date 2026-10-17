# HPOP 通用变量 VOP 积分

[STK 积分设置](https://help.agi.com/stk/Content/hpop/hpopIntegrator.htm)把 VOP 作为运动方程选项，仅与 RKF7(8)、Bulirsch–Stoer 组合。OrbitForge 使用同样的选项关系：`HpopOptions.vop` / Python `Hpop(vop=True)` 默认关闭；RK4、RKV89、Gauss–Jackson 保存但忽略该草稿。桌面积分器页按方法启用复选框。VOP 积分使用所选引力模型的 `gravity_mu`，与独立的插值方法及插值 μ 无关。

## 方程与误差控制

每个候选步在起点重新定义瞬时 Cartesian 参数 `c`，令 `y(t)=Phi(t-t0,c(t))`，其中 `Phi` 是通用变量二体映射。中心引力由映射解析推进，积分器推进摄动引起的参数变化：`c_dot = D(Phi)^(-1) [0,a_pert]`。由二体流的辛性质，位置 Jacobian 的两个块 `A=dR/dr0`、`B=dR/dv0` 给出 `c_dot=(-B^T a_pert,A^T a_pert)`。完整力模型仍在每个物理阶段的实际位置、速度、TAI 求值。

通用异常量采用半径缩放的 Stumpff 函数及有界求根。Jacobian 对已收敛方程作隐式求导，不差分轨迹，也不微分迭代次数；周期归约保留周期对初态的导数，整圈和多圈状态转移矩阵不会错误退化为单位阵。可表示的非径向椭圆、抛物线和双曲线均支持；数值无法分辨角动量的状态明确拒绝。

RKF78 仍推进七阶解、使用八阶估计；BS 保留现有中点/Richardson 序列、阶数与步长控制。参数缺陷通过完整 STM 映射到物理位置/速度，再使用原有 `absolute + relative * max(abs(before),abs(after))` 尺度。固定步长没有误差拒绝。BS 外推局部增量，二体映射单独保留增量以减少大坐标相减损失。

## 正则化、分段与状态

时间正则化的第七维仍为物理时间，映射时间采用该状态的实际变化；摄动乘以 `dt/dq`，误差映射包含时间误差引起的二体位置/速度变化。物理时间边界桥接的第七维为 `q`，使用普通时间形式。Sundman 每圈归一化仍要求椭圆，不能因为启用 VOP 而接受非束缚轨道的正则化。

天气跳变、阴影接触、报告时间及机动保持已有分段规则；参数在候选步起点重置，脉冲后的新滑行从机动后状态起步。最低高度检查作用于实际力求值状态。取消、拒绝步及失败不发布局部参数或未完成的缓存。

当前文件 v40 / OBJS31 在积分器枚举之后存储严格 0/1 的 VOP 字节。非活动草稿精确保留；分析、报告和 Session 身份只采用有效开关。有效 VOP 来源为 `hpop-vop-universal-osculating-gcrf-tai-v1`，Cowell 标识不变。文件仅读写当前版本。

## 验证及限制

见 [数值对照协议](../validation/vop_integration_protocol.md)。STK 没有在公开帮助中披露其内部参数定义、控制器或全部实现细节；这里对齐公开功能语义，并以独立 Orekit/Hipparchus、解析解和几何不变量验证。没有本机 STK，不声称与 STK 逐位一致或已完成同配置验收。
