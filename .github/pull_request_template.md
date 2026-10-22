## 这个 PR 做了什么

<!-- 一两句话。细节留给代码和 commit message。 -->

## 为什么

<!-- 关联 issue：Closes #123 -->

## 自查清单

- [ ] `clang-format` 已跑过（提交里不应有纯格式 diff）
- [ ] 构建零告警（`-DRELAYER_WERROR=ON` 通过）
- [ ] `ctest` 全过，包括 `arch` 用例
- [ ] 新逻辑有对应测试；修 bug 的 PR 有能复现原问题的用例
- [ ] 没有引入新的全局状态或单例
- [ ] 若改了协议 / 依赖方向 / 公开接口，已同步更新 `docs/` 下对应文档
- [ ] 若新增三方依赖，已更新 `third_party/README.md` 的版本表

## 需要 reviewer 特别注意的地方

<!-- 例如：这里我绕开了一个 std::filesystem 的坑（链接到 issue）；
     这个循环的边界条件我推了两遍仍不确定；
     这段性能敏感，请不要改成 xxx 写法。 -->
