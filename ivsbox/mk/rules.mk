# 通用编译、归档、链接规则
# 顶层 Makefile 通过 include 引入，不再使用递归 make

# 确保静默模式不会隐藏真正有用的命令；需要时可用 make V=1 显示完整命令
ifeq ($(V),1)
Q :=
else
Q := @
endif

# 统一模式规则在顶层 Makefile 中按目录声明，此处保留扩展钩子
# 未来可在此追加：
#   - 版本头生成规则
#   - 依赖第三方库的 pkg-config 查询
#   - 产物大小/符号检查

.SUFFIXES:
.SUFFIXES: .c .o .a
