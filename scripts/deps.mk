ifeq ($(DEBUG),y)
INTERN_CFLAGS+=-MT $@ -MMD -MP -MF $(objdir)$*.d
endif
deps-target=$(filter %.d,$(objs-target:%.o=%.d))
objs-target+=$(deps-target)
include $(wildcard $(deps-target))

