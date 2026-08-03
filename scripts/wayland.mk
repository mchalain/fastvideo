WL_PROTOCOLS_DIR:=$(call cmd_pkgconfig,wayland-protocols,--variable=pkgdatadir)
WL_FLAGS:=$(call cmd_pkgconfig,wayland-client,--cflags --libs)
WL_SCANNER:=$(call cmd_pkgconfig,wayland-scanner,--variable=wayland_scanner)

$(foreach t,$(slib-y) $(lib-y) $(bin-y) $(sbin-y) $(modules-y) $(hostslib-y) $(hostbin-y), $(eval $(t)_GENERATED+=$(patsubst %,%.wlext.c,$(filter %.xml,$($(t)_WAYLANDEXT) $($(t)_WAYLANDEXT-y)))))
$(foreach t,$(slib-y) $(lib-y) $(bin-y) $(sbin-y) $(modules-y) $(hostslib-y) $(hostbin-y), $(eval $(t)_GENERATED+=$(patsubst %,%.wliext.h,$(filter %.xml,$($(t)_WAYLANDEXT) $($(t)_WAYLANDEXT-y)))))
$(foreach t,$(slib-y) $(lib-y) $(bin-y) $(sbin-y) $(modules-y) $(hostslib-y) $(hostbin-y), $(eval $(t)-objs+=$(addsuffix .o,$(call notext,$(filter %.wlext.c,$($(t)_GENERATED))))))

$(foreach t,$(slib-y) $(lib-y) $(bin-y) $(sbin-y) $(modules-y) $(hostslib-y) $(hostbin-y), $(eval $(addprefix $(objdir),$($(t)-objs)): | $(addprefix $(objdir),$(filter %.wliext.h,$($(t)_GENERATED)))))

quiet_cmd_wlscanh=WLSCANNER $*
define  cmd_wlscanh
	$(WL_SCANNER) client-header $< $@
endef
define  cmd_wlscanc
	$(WL_SCANNER) private-code $< $@
endef

$(objdir)%.wlext.c: $(WL_PROTOCOLS_DIR)/% $(file)
	@mkdir -p $(dir $@)
	@$(call cmd,wlscanc)

$(objdir)%.wliext.h: $(WL_PROTOCOLS_DIR)/% $(file)
	@mkdir -p $(dir $@)
	@$(call cmd,wlscanh)

_help_entries_wlext:
	@echo " <target>_WAYLANDEXT-y+=<file>.xml  example: stable/xdg-shell/xdg-shell.xml (relative to wayland-protocols' pkgdatadir)"

_help_options_wlext:
	@
