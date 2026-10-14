/* omxprobe: boot the DSP with Sony/NEC's own firmware through NEC's OpenMAX IL 1.0 core,
 * enumerate components and instantiate one decoder.  gemini MUST NOT be running
 * (only one process can own /dev/InterDSP/control and the DSP).
 * Usage: omxprobe [cfg] [component]
 *   default cfg       = /usr/lib/omf/omx_nw_walkman_audio_ftb.cfg
 *   default component = OMX.NEC.EL.MP3D.FTB
 * Linked directly against the device libs in /usr/lib/omf (see build.sh), no dlopen needed. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

typedef uint32_t OMX_ERRORTYPE;
typedef void *OMX_HANDLETYPE;

typedef struct {		/* OMX IL 1.0 callbacks */
	OMX_ERRORTYPE (*EventHandler)(OMX_HANDLETYPE, void *, int, uint32_t, uint32_t, void *);
	OMX_ERRORTYPE (*EmptyBufferDone)(OMX_HANDLETYPE, void *, void *);
	OMX_ERRORTYPE (*FillBufferDone)(OMX_HANDLETYPE, void *, void *);
} OMX_CALLBACKTYPE;

typedef struct {		/* OMX IL 1.0 OMX_COMPONENTTYPE: 16 fn pointers @+0x10..+0x4c (verified in omf_mc_mp3d.so) */
	uint32_t nSize, nVersion;
	void *pComponentPrivate, *pApplicationPrivate;
	OMX_ERRORTYPE (*GetComponentVersion)(OMX_HANDLETYPE, char *, uint32_t *, uint32_t *, uint8_t (*)[128]);
	void *SendCommand, *GetParameter, *SetParameter, *GetConfig, *SetConfig, *GetExtensionIndex;
	OMX_ERRORTYPE (*GetState)(OMX_HANDLETYPE, int *);
	void *ComponentTunnelRequest, *UseBuffer, *AllocateBuffer, *FreeBuffer;
	void *EmptyThisBuffer, *FillThisBuffer, *SetCallbacks, *ComponentDeInit;
} OMX_COMPONENTTYPE;

/* NEC core. NOTE: OMX_Init takes the cfg file path (non-standard; verified in libomx_core.so:
 * OMX_Init -> OmfInit(r0), NULL path => error). */
extern OMX_ERRORTYPE OMX_Init(const char *cfg);
extern OMX_ERRORTYPE OMX_Deinit(void);
extern OMX_ERRORTYPE OMX_ComponentNameEnum(char *name, uint32_t len, uint32_t index);
extern OMX_ERRORTYPE OMX_GetHandle(OMX_HANDLETYPE *h, const char *name, void *app, OMX_CALLBACKTYPE *cb);
extern OMX_ERRORTYPE OMX_FreeHandle(OMX_HANDLETYPE h);

static OMX_ERRORTYPE ev(OMX_HANDLETYPE h, void *a, int e, uint32_t d1, uint32_t d2, void *d)
{
	printf("  event %d %u %u\n", e, d1, d2);
	return 0;
}
static OMX_ERRORTYPE ebd(OMX_HANDLETYPE h, void *a, void *b) { return 0; }
static OMX_ERRORTYPE fbd(OMX_HANDLETYPE h, void *a, void *b) { return 0; }

static void irqline(const char *when)
{
	char l[256];
	FILE *f = fopen("/proc/interrupts", "r");
	if (!f)
		return;
	while (fgets(l, sizeof l, f))
		if (strstr(l, "interdsp"))
			printf("[%s] %s", when, l);
	fclose(f);
}

int main(int argc, char **argv)
{
	const char *cfg = argc > 1 ? argv[1] : "/usr/lib/omf/omx_nw_walkman_audio_ftb.cfg";
	const char *comp = argc > 2 ? argv[2] : "OMX.NEC.EL.MP3D.FTB";
	char name[128];
	uint32_t i;
	OMX_ERRORTYPE r;
	OMX_HANDLETYPE h = 0;
	OMX_CALLBACKTYPE cb = { ev, ebd, fbd };

	setvbuf(stdout, 0, _IONBF, 0);
	irqline("before");
	r = OMX_Init(cfg);	/* loads omf_dsp_manager.mp201, INTERDSP_DOWNLOAD, releases DSP reset */
	printf("OMX_Init(%s) = %#x\n", cfg, r);
	irqline("after init");
	if (r)
		return 1;
	for (i = 0; i < 32; i++) {
		memset(name, 0, sizeof name);
		if (OMX_ComponentNameEnum(name, sizeof name, i))
			break;
		printf("  component[%u] = %s\n", i, name);
	}
	r = OMX_GetHandle(&h, comp, 0, &cb);	/* loads the codec's ME overlay on the DSP */
	printf("OMX_GetHandle(%s) = %#x handle=%p\n", comp, r, h);
	if (!r && h) {
		OMX_COMPONENTTYPE *c = h;
		char cn[128];
		uint32_t cv = 0, sv = 0;
		uint8_t uuid[128];
		int st = -1;
		if (c->GetComponentVersion && !c->GetComponentVersion(h, cn, &cv, &sv, &uuid))
			printf("  name=%s compVer=%08x specVer=%08x\n", cn, cv, sv);
		if (c->GetState && !c->GetState(h, &st))
			printf("  state=%d (1=Loaded)\n", st);
		irqline("after gethandle");
		printf("OMX_FreeHandle = %#x\n", OMX_FreeHandle(h));
	}
	printf("OMX_Deinit = %#x\n", OMX_Deinit());
	irqline("end");
	return 0;
}
