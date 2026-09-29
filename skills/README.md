# TurboScript Development Skills

本目录包含 TurboScript 项目的专项技术规范，作为 AGENTS.md 核心约束的补充。

## 可用 Skills

1. **[turbonet_utils.md](turbonet_utils.md)** - TurboNet Utils 完整 API 参考
   - 内存管理（Slab 分配器、对象池、Arena）
   - 字符串处理（tstr_t、tstr_v）
   - 文件系统（turbo_fs、turbo_mmap）
   - 日志系统（tlog）
   - 并发原语（线程、锁、线程池）
   - 无锁数据结构（Disruptor、环形缓冲区、桶式优先队列）
   - 使用场景映射表、性能指标

2. **[c_design_patterns.md](c_design_patterns.md)** - C 语言设计模式实现指南
   - 创建型模式：工厂、建造者、单例
   - 结构型模式：适配器、桥接、组合、装饰器
   - 行为型模式：策略、观察者、命令、访问者、模板方法
   - C 语言最佳实践、SOLID 原则、反模式警告

3. **[performance_optimization.md](performance_optimization.md)** - 性能优化专项指南
   - 热路径识别与优化
   - 算法复杂度约束
   - 内存管理优化（批量分配、对象池、缓存对齐）
   - SIMD 与向量化、缓存优化、并发优化
   - 性能测试框架、优化检查清单

4. **[logging_guide.md](logging_guide.md)** - 日志系统最佳实践
   - 日志数量与性能约束（量化标准）
   - 日志内容质量规范
   - 日志级别使用规范
   - 文件管理（滚动、归档、保留策略）
   - 生产环境配置、审查检查清单

5. **[plugin_system.md](plugin_system.md)** - 插件系统开发规范
   - 插件架构设计（生命周期、版本管理）
   - 插件接口设计（稳定 ABI）
   - 插件隔离机制（内存、资源、权限、崩溃）
   - 插件依赖管理（拓扑排序、可选依赖）
   - 插件通信（事件总线、服务注册）
   - 热重载、安全机制、测试与调试

6. **[tinytest.md](tinytest.md)** - TinyTest 测试框架指南
   - C/C++ 单元测试结构（suite/spec/group/it）
   - fixture、typed assertions、临时文件 helper
   - 过滤、TAP、JUnit、benchmark 使用规范

## 使用方式
 
在对话中使用 `#` 引用 skill 文件名：
```
#skills/turbonet_utils.md
#skills/c_design_patterns.md
#skills/performance_optimization.md
#skills/logging_guide.md
#skills/plugin_system.md
#skills/tinytest.md
``` 

## 维护原则

- **独立性**：每个 skill 独立维护，不依赖其他 skills
- **完整性**：每个 skill 必须包含完整的规范、示例和最佳实践
- **可执行性**：所有示例代码必须可编译、可运行
- **量化标准**：性能、日志等约束必须量化，不用模糊描述
- **与主文件分离**：核心约束在 AGENTS.md，详细实现在 skills

## 贡献指南

新增或修改 skill 时，请确保：
1. 文件名使用小写字母 + 下划线
2. 内容结构清晰，使用 Markdown 标准格式
3. 包含具体示例代码和使用场景
4. 更新本 README.md 的 skill 列表
5. 在 AGENTS.md 相应章节添加引用
