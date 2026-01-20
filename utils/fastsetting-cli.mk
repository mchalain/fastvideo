bin-y+=fastsetting_cli
fastsetting_cli_SOURCES+=fastsetting-cli.c
fastsetting_cli_SOURCES+=fastsetting-ui.c
fastsetting_cli_SOURCES+=$(srcdir)src/client.c
fastsetting_cli_LIBRARY+=ncurses
fastsetting_cli_LIBRARY+=jansson
fastsetting_cli_CFLAGS+=-I$(srcdir)src
fastsetting_cli_LDFLAGS+=-L$(builddir)src
