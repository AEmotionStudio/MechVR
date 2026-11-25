#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h> // openxr_platform.h names IUnknown for Windows
#define XR_USE_PLATFORM_WIN32
#define XR_NO_PROTOTYPES // the OpenXR loader is opened when the headset is wanted, so --flat runs without it
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define XR_USE_PLATFORM_XLIB
#define sleep_ms(ms) usleep((ms) * 1000)
#endif
#include "xr.h"
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
// Every OpenXR call the game makes, looked up in openxr_loader.dll (next to mechvr.exe, or on the PATH).
#define XR_FUNCS(X) X(xrAcquireSwapchainImage) X(xrApplyHapticFeedback) X(xrAttachSessionActionSets) X(xrBeginFrame) \
    X(xrBeginSession) X(xrCreateAction) X(xrCreateActionSet) X(xrCreateActionSpace) X(xrCreateInstance)              \
    X(xrCreateReferenceSpace) X(xrCreateSession) X(xrCreateSwapchain) X(xrDestroyInstance) X(xrDestroySession)       \
    X(xrEndFrame) X(xrEndSession) X(xrEnumerateInstanceExtensionProperties) X(xrEnumerateReferenceSpaces)          \
    X(xrEnumerateSwapchainFormats) X(xrEnumerateSwapchainImages) X(xrEnumerateViewConfigurationViews)              \
    X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStateVector2f) X(xrGetInstanceProcAddr)       \
    X(xrGetCurrentInteractionProfile) X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties)           \
    X(xrLocateSpace) X(xrLocateViews) X(xrPathToString) X(xrPollEvent) X(xrReleaseSwapchainImage)                  \
    X(xrResultToString) X(xrStringToPath)                                                                          \
    X(xrSuggestInteractionProfileBindings) X(xrSyncActions) X(xrWaitFrame) X(xrWaitSwapchainImage)
#define XR_DECLARE(f) static PFN_##f f;
XR_FUNCS(XR_DECLARE)
#undef XR_DECLARE

static int xr_load(void) {
    HMODULE m = LoadLibraryA("openxr_loader.dll");
    if (!m) m = LoadLibraryA("libopenxr_loader.dll"); // the MSYS2 package's name for it
    if (!m) {
        fprintf(stderr, "xr: openxr_loader.dll not found. Put it next to mechvr.exe (it comes with the Windows download\n"
                        "    and with the Khronos OpenXR SDK), or play in a window with --flat.\n");
        return 0;
    }
#define XR_LOAD(f)                                                                  \
    if (!(f = (PFN_##f)(void (*)(void))GetProcAddress(m, #f))) {                    \
        fprintf(stderr, "xr: openxr_loader.dll has no %s\n", #f);                   \
        return 0;                                                                   \
    }
    XR_FUNCS(XR_LOAD)
#undef XR_LOAD
    return 1;
}
#endif

#define MAX_IMG 8
static XrInstance inst;
static XrSystemId sys;
static XrSession ses;
static XrSpace space, viewSpace;
static XrSwapchain sc[2];
static XrSwapchainImageOpenGLKHR img[2][MAX_IMG];
static int scW[2], scH[2];
static XrSessionState state;
static int running, srgb, quitting;
static XrFrameState fstate;
static XrView views[2];
static XrCompositionLayerProjectionView pviews[2];

static XrActionSet aset;
static XrAction aGrip, aAim, aTrig, aSqueeze, aStick, aBtnA, aBtnB, aMenu, aHaptic;
static XrPath handPath[2];
static XrSpace gripSpace[2], aimSpace[2];
static int prevA[2], prevB[2];

static int ok(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return 1;
    char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (inst) xrResultToString(inst, r, buf);
    fprintf(stderr, "xr: %s failed: %s (%d)\n", what, buf, (int)r);
    return 0;
}
#define TRY(call) do { if (!ok(call, #call)) return 0; } while (0)

static xf toXf(XrPosef p) {
    return XF(V3(p.position.x, p.position.y, p.position.z),
              Q(p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w));
}
static XrPath path(const char *s) { XrPath p = XR_NULL_PATH; xrStringToPath(inst, s, &p); return p; }

static XrAction mkAction(const char *name, XrActionType type) {
    XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    strcpy(ci.actionName, name);
    strcpy(ci.localizedActionName, name);
    ci.countSubactionPaths = 2;
    ci.subactionPaths = handPath;
    XrAction a = XR_NULL_HANDLE;
    ok(xrCreateAction(aset, &ci, &a), name);
    return a;
}

static int suggest(const char *profile, XrActionSuggestedBinding *b, int n) {
    XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path(profile);
    sb.suggestedBindings = b;
    sb.countSuggestedBindings = n;
    return ok(xrSuggestInteractionProfileBindings(inst, &sb), profile);
}

// The controllers the game has bindings for. Each layout names the input behind each game action, per hand
// ([0] left, [1] right; 0 leaves it unbound). The grip and aim poses and the rumble are the same on all of them.
// a is the lower face button (X on a left Touch controller, A on a right one) and b the upper (Y, B); the menu
// button is read on the left hand only. Layouts with an extension are used only when the runtime offers it.
typedef struct {
    const char *profile, *ext;
    const char *trigger, *squeeze, *stick, *a[2], *b[2], *menu;
} Layout;
#define TOUCH_BUTTONS {"x/click", "a/click"}, {"y/click", "b/click"}, "menu/click" // the Touch layout of face buttons
static const Layout layouts[] = {
    {"oculus/touch_controller", 0, "trigger/value", "squeeze/value", "thumbstick", TOUCH_BUTTONS}, // Rift, every Quest
    {"valve/index_controller", 0, "trigger/value", "squeeze/value", "thumbstick", // grab by closing the hand
     {"a/click", "a/click"}, {"b/click", "b/click"}, "thumbstick/click"},
    {"htc/vive_controller", 0, "trigger/value", "squeeze/click", "trackpad", // the trackpads stand in for thumbsticks
     {0, "trackpad/click"}, {"trackpad/click", "menu/click"}, "menu/click"},
    {"microsoft/motion_controller", 0, "trigger/value", "squeeze/click", "thumbstick", // Windows Mixed Reality
     {0, "trackpad/click"}, {"trackpad/click", "menu/click"}, "menu/click"},
    {"hp/mixed_reality_controller", "XR_EXT_hp_mixed_reality_controller", "trigger/value", "squeeze/value", "thumbstick", TOUCH_BUTTONS},
    {"bytedance/pico4_controller", "XR_BD_controller_interaction", "trigger/value", "squeeze/value", "thumbstick", TOUCH_BUTTONS},
    {"htc/vive_cosmos_controller", "XR_HTC_vive_cosmos_controller_interaction", "trigger/value", "squeeze/click", "thumbstick", TOUCH_BUTTONS},
    {"htc/vive_focus3_controller", "XR_HTC_vive_focus3_controller_interaction", "trigger/value", "squeeze/value", "thumbstick", TOUCH_BUTTONS},
    {"khr/generic_controller", "XR_KHR_generic_controller", "trigger/value", "squeeze/value", "thumbstick", // any other controller
     {"primary/click", "primary/click"}, {"secondary/click", "secondary/click"}, "thumbstick/click"},
    {"khr/simple_controller", 0, "select/click", 0, 0, {0, 0}, {0, 0}, "menu/click"}, // the last resort: point and click
};
#define NLAYOUT (sizeof layouts / sizeof layouts[0])
static int layoutOn[NLAYOUT]; // 1 when the layout's extension was enabled

static int initActions(void) {
    handPath[0] = path("/user/hand/left");
    handPath[1] = path("/user/hand/right");
    XrActionSetCreateInfo si = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "game");
    strcpy(si.localizedActionSetName, "Game");
    TRY(xrCreateActionSet(inst, &si, &aset));
    aGrip = mkAction("grip", XR_ACTION_TYPE_POSE_INPUT);
    aAim = mkAction("aim", XR_ACTION_TYPE_POSE_INPUT);
    aTrig = mkAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    aSqueeze = mkAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT);
    aStick = mkAction("stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    aBtnA = mkAction("button_a", XR_ACTION_TYPE_BOOLEAN_INPUT);
    aBtnB = mkAction("button_b", XR_ACTION_TYPE_BOOLEAN_INPUT);
    aMenu = mkAction("menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    aHaptic = mkAction("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);

    // One set of bindings per controller layout; the runtime uses the one that fits the controllers in use.
    for (unsigned i = 0; i < NLAYOUT; i++) {
        const Layout *L = &layouts[i];
        if (L->ext && !layoutOn[i]) continue; // the runtime does not offer this layout
        XrActionSuggestedBinding b[18];
        int n = 0;
        for (int h = 0; h < 2; h++) {
            const char *in[] = {"grip/pose", "aim/pose", L->trigger, L->squeeze, L->stick, L->a[h], L->b[h], h ? 0 : L->menu};
            XrAction act[] = {aGrip, aAim, aTrig, aSqueeze, aStick, aBtnA, aBtnB, aMenu};
            char p[96];
            for (int k = 0; k < 8; k++)
                if (in[k]) {
                    snprintf(p, sizeof p, "%s/input/%s", h ? "/user/hand/right" : "/user/hand/left", in[k]);
                    b[n++] = (XrActionSuggestedBinding){act[k], path(p)};
                }
            b[n++] = (XrActionSuggestedBinding){aHaptic, path(h ? "/user/hand/right/output/haptic" : "/user/hand/left/output/haptic")};
        }
        char prof[96];
        snprintf(prof, sizeof prof, "/interaction_profiles/%s", L->profile);
        suggest(prof, b, n);
    }

    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sp = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sp.subactionPath = handPath[h];
        sp.poseInActionSpace.orientation.w = 1;
        sp.action = aGrip;
        TRY(xrCreateActionSpace(ses, &sp, &gripSpace[h]));
        sp.action = aAim;
        TRY(xrCreateActionSpace(ses, &sp, &aimSpace[h]));
    }
    XrSessionActionSetsAttachInfo at = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &aset;
    TRY(xrAttachSessionActionSets(ses, &at));
    return 1;
}

int xr_init(Gfx *g) {
#ifdef _WIN32
    if (!xr_load()) return 0;
#endif
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
    XrExtensionProperties ext[256];
    if (n > 256) n = 256;
    for (uint32_t i = 0; i < n; i++) ext[i] = (XrExtensionProperties){XR_TYPE_EXTENSION_PROPERTIES};
    xrEnumerateInstanceExtensionProperties(NULL, n, &n, ext);
    int haveGL = 0;
    const char *exts[1 + NLAYOUT] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
    uint32_t nexts = 1;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(ext[i].extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME)) haveGL = 1;
        for (unsigned l = 0; l < NLAYOUT; l++) // controller layouts the runtime offers as extensions
            if (layouts[l].ext && !layoutOn[l] && !strcmp(ext[i].extensionName, layouts[l].ext)) {
                layoutOn[l] = 1;
                exts[nexts++] = layouts[l].ext;
            }
    }
    if (!haveGL) { fprintf(stderr, "xr: runtime has no OpenGL support (or no runtime is active)\n"); return 0; }

    XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "MechVR");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = nexts;
    ici.enabledExtensionNames = exts;
    TRY(xrCreateInstance(&ici, &inst));
    XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(inst, &ip);

    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    TRY(xrGetSystem(inst, &sgi, &sys));
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(inst, sys, &sp);
    printf("xr: runtime %s, system %s\n", ip.runtimeName, sp.systemName);

    PFN_xrGetOpenGLGraphicsRequirementsKHR getReq = NULL;
    TRY(xrGetInstanceProcAddr(inst, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getReq));
    XrGraphicsRequirementsOpenGLKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    TRY(getReq(inst, sys, &req));

#ifdef _WIN32
    XrGraphicsBindingOpenGLWin32KHR gb = {XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    gb.hDC = (HDC)g->dc;
    gb.hGLRC = (HGLRC)g->ctx;
#else
    XrGraphicsBindingOpenGLXlibKHR gb = {XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
    gb.xDisplay = g->dpy;
    gb.visualid = (uint32_t)g->visualid;
    gb.glxFBConfig = g->fbc;
    gb.glxDrawable = g->win;
    gb.glxContext = g->ctx;
#endif
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = sys;
    TRY(xrCreateSession(inst, &sci, &ses));

    // Prefer a floor-level space so standing height is real; fall back to seated-local.
    XrReferenceSpaceType types[16];
    uint32_t nt = 0;
    xrEnumerateReferenceSpaces(ses, 16, &nt, types);
    XrReferenceSpaceType want = XR_REFERENCE_SPACE_TYPE_LOCAL;
    for (uint32_t i = 0; i < nt; i++)
        if (types[i] == XR_REFERENCE_SPACE_TYPE_STAGE) want = XR_REFERENCE_SPACE_TYPE_STAGE;
    XrReferenceSpaceCreateInfo rci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rci.poseInReferenceSpace.orientation.w = 1;
    rci.referenceSpaceType = want;
    TRY(xrCreateReferenceSpace(ses, &rci, &space));
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    TRY(xrCreateReferenceSpace(ses, &rci, &viewSpace));
    printf("xr: play space %s\n", want == XR_REFERENCE_SPACE_TYPE_STAGE ? "stage (floor level)" : "local (seated)");

    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t nv = 0;
    TRY(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vcv));

    int64_t fmts[64];
    uint32_t nf = 0;
    TRY(xrEnumerateSwapchainFormats(ses, 64, &nf, fmts));
    static const int64_t prefer[] = {GL_SRGB8_ALPHA8, GL_RGBA8, GL_RGB10_A2, GL_RGBA16F};
    int64_t fmt = 0;
    for (unsigned p = 0; p < sizeof prefer / sizeof prefer[0] && !fmt; p++)
        for (uint32_t i = 0; i < nf; i++)
            if (fmts[i] == prefer[p]) { fmt = prefer[p]; break; }
    printf("xr: swapchain formats offered:");
    for (uint32_t i = 0; i < nf; i++) printf(" 0x%llX", (long long)fmts[i]);
    if (!fmt && nf) fmt = fmts[0];
    printf("\nxr: using format 0x%llX\n", (long long)fmt);
    srgb = fmt == GL_SRGB8_ALPHA8;

    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = fmt;
        ci.sampleCount = 1;
        ci.width = vcv[e].recommendedImageRectWidth;
        ci.height = vcv[e].recommendedImageRectHeight;
        ci.faceCount = ci.arraySize = ci.mipCount = 1;
        TRY(xrCreateSwapchain(ses, &ci, &sc[e]));
        scW[e] = ci.width;
        scH[e] = ci.height;
        uint32_t ni = 0;
        for (int i = 0; i < MAX_IMG; i++) img[e][i] = (XrSwapchainImageOpenGLKHR){XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR};
        TRY(xrEnumerateSwapchainImages(sc[e], MAX_IMG, &ni, (XrSwapchainImageBaseHeader *)img[e]));
    }
    printf("xr: eye image %dx%d\n", scW[0], scH[0]);
    return initActions();
}

int xr_running(void) { return running; }
int xr_srgb(void) { return srgb; }

int xr_poll(void) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            state = ((XrEventDataSessionStateChanged *)&ev)->state;
            if (state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                running = ok(xrBeginSession(ses, &bi), "xrBeginSession");
            } else if (state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(ses);
                running = 0;
            } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                quitting = 1;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            quitting = 1;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) { // say which bindings are in use
            for (int h = 0; h < 2; h++) {
                XrInteractionProfileState ps = {XR_TYPE_INTERACTION_PROFILE_STATE};
                char name[XR_MAX_PATH_LENGTH] = "none";
                uint32_t len = 0;
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(ses, handPath[h], &ps)) && ps.interactionProfile)
                    xrPathToString(inst, ps.interactionProfile, sizeof name, &len, name);
                printf("xr: %s hand: %s\n", h ? "right" : "left", name);
            }
        }
        ev = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
    }
    return !quitting;
}

static float getF(XrAction a, int h) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = handPath[h];
    XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};
    xrGetActionStateFloat(ses, &gi, &s);
    return s.isActive ? s.currentState : 0;
}
static int getB(XrAction a, int h) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = handPath[h];
    XrActionStateBoolean s = {XR_TYPE_ACTION_STATE_BOOLEAN};
    xrGetActionStateBoolean(ses, &gi, &s);
    return s.isActive && s.currentState;
}
static int locate(XrSpace s, XrTime t, xf *out) {
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(s, space, t, &loc))) return 0;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return 0;
    *out = toXf(loc.pose);
    return 1;
}

void xr_begin(XrFrame *f) {
    memset(f, 0, sizeof *f);
    f->head = xfid();
    if (!running) { sleep_ms(20); return; }
    fstate = (XrFrameState){XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(ses, NULL, &fstate))) return;
    xrBeginFrame(ses, NULL);
    f->dt = fstate.predictedDisplayPeriod * 1e-9;
    f->render = fstate.shouldRender;

    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fstate.predictedDisplayTime;
    li.space = space;
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    views[0] = views[1] = (XrView){XR_TYPE_VIEW};
    uint32_t nv = 0;
    if (XR_FAILED(xrLocateViews(ses, &li, &vs, 2, &nv, views)) ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        f->render = 0;
    for (int e = 0; e < 2 && f->render; e++) {
        f->eye[e].pose = toXf(views[e].pose);
        f->eye[e].l = tanf(views[e].fov.angleLeft);
        f->eye[e].r = tanf(views[e].fov.angleRight);
        f->eye[e].d = tanf(views[e].fov.angleDown);
        f->eye[e].u = tanf(views[e].fov.angleUp);
        f->eye[e].w = scW[e];
        f->eye[e].h = scH[e];
    }
    locate(viewSpace, fstate.predictedDisplayTime, &f->head);

    if (state == XR_SESSION_STATE_FOCUSED) {
        XrActiveActionSet as = {aset, XR_NULL_PATH};
        XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &as;
        xrSyncActions(ses, &si);
        for (int h = 0; h < 2; h++) {
            XrHand *hd = &f->hand[h];
            hd->grip = hd->aim = xfid();
            hd->tracked = locate(gripSpace[h], fstate.predictedDisplayTime, &hd->grip) &&
                          locate(aimSpace[h], fstate.predictedDisplayTime, &hd->aim);
            hd->trigger = getF(aTrig, h);
            hd->squeeze = getF(aSqueeze, h);
            XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = aStick;
            gi.subactionPath = handPath[h];
            XrActionStateVector2f st = {XR_TYPE_ACTION_STATE_VECTOR2F};
            xrGetActionStateVector2f(ses, &gi, &st);
            if (st.isActive) { hd->sx = st.currentState.x; hd->sy = st.currentState.y; }
            hd->a = getB(aBtnA, h);
            hd->b = getB(aBtnB, h);
            hd->aDown = hd->a && !prevA[h];
            hd->bDown = hd->b && !prevB[h];
            prevA[h] = hd->a;
            prevB[h] = hd->b;
        }
        f->menu = getB(aMenu, 0);
    }
}

void xr_eye_acquire(XrFrame *f, int e) {
    uint32_t idx = 0;
    xrAcquireSwapchainImage(sc[e], NULL, &idx);
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc[e], &wi);
    f->eye[e].tex = img[e][idx].image;
}
void xr_eye_release(int e) { xrReleaseSwapchainImage(sc[e], NULL); }

void xr_end(XrFrame *f) {
    if (!running || !fstate.type) return;
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader *layers[1] = {(const XrCompositionLayerBaseHeader *)&layer};
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fstate.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (f->render) {
        for (int e = 0; e < 2; e++) {
            pviews[e] = (XrCompositionLayerProjectionView){XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            pviews[e].pose = views[e].pose;
            pviews[e].fov = views[e].fov;
            pviews[e].subImage.swapchain = sc[e];
            pviews[e].subImage.imageRect.extent.width = scW[e];
            pviews[e].subImage.imageRect.extent.height = scH[e];
        }
        layer.space = space;
        layer.viewCount = 2;
        layer.views = pviews;
        ei.layerCount = 1;
        ei.layers = layers;
    }
    xrEndFrame(ses, &ei);
}

void xr_haptic(int hand, float amplitude, float seconds) {
    if (!running) return;
    XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amplitude;
    v.duration = (XrDuration)(seconds * 1e9);
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = aHaptic;
    hi.subactionPath = handPath[hand];
    xrApplyHapticFeedback(ses, &hi, (const XrHapticBaseHeader *)&v);
}

void xr_shutdown(void) {
    if (ses) xrDestroySession(ses);
    if (inst) xrDestroyInstance(inst);
    ses = XR_NULL_HANDLE;
    inst = XR_NULL_HANDLE;
}
