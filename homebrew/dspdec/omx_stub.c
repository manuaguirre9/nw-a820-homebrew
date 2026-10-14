/* Link-time stand-ins for the device's NEC OpenMAX core (/usr/lib/omf/libomx_core.so), so dspdec builds without
 * any Sony/NEC file. build.sh compiles this into stub libomx_core.so / libomf_core_hw_dep.so / libomf_core_os_dep.so
 * that only give the linker the symbol names; at run time the dynamic loader picks up the real libraries on the
 * device (rpath /usr/lib/omf). Never copied to the device. */
int OMX_DriverMode(int *mode) { (void)mode; return -1; }
int OMX_Init(const char *cfg) { (void)cfg; return -1; }
int OMX_Deinit(void) { return -1; }
int OMX_GetHandle(void **h, const char *name, void *app, void *cb) { (void)h; (void)name; (void)app; (void)cb; return -1; }
int OMX_FreeHandle(void *h) { (void)h; return -1; }
