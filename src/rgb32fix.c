/*
 * MK11 RGB32 buffer fix.
 *
 * MK11 reads per-vertex face data through typed buffer SRVs in
 * DXGI_FORMAT_R32G32B32_* format. Metal has no 3x32-bit texel buffer format,
 * so DXVK, DXMT and D3DMetal all mishandle these views and faces render
 * garbled. This DLL (loaded as a proxy xinput9_1_0.dll) hooks D3D11 and
 * transparently replaces every such view with an R32G32B32A32 view over a
 * shadow buffer in which each 12-byte element is padded to 16 bytes. Writes
 * to the original buffer (UpdateSubresource, Map/Unmap, copies) are mirrored
 * into the shadow.
 *
 * It also fixes cutscenes. MK11 decodes its movies with the Media Foundation
 * H.264 decoder. Under CrossOver on macOS that decoder breaks them in two ways:
 * it hides NV12, the only output format MK11 can draw, and it returns frames
 * in decode order instead of display order. The hooks offer NV12 again and put
 * the frames back in display order.
 */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <initguid.h>
#include <d3d11_4.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>

/* ---------------------------------------------------------------- logging */

static CRITICAL_SECTION g_log_cs;
static HANDLE g_log = INVALID_HANDLE_VALUE;

static void log_msg(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    SYSTEMTIME t;
    int n;
    DWORD written;

    if (g_log == INVALID_HANDLE_VALUE)
        return;
    GetLocalTime(&t);
    n = snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_log_cs);
    WriteFile(g_log, buf, n, &written, NULL);
    LeaveCriticalSection(&g_log_cs);
}

static void open_log(HMODULE self)
{
    char path[MAX_PATH];
    char *slash;

    InitializeCriticalSection(&g_log_cs);
    GetModuleFileNameA(self, path, sizeof(path));
    slash = strrchr(path, '\\');
    if (slash)
        strcpy(slash + 1, "mk11_rgb32fix.log");
    else
        strcpy(path, "mk11_rgb32fix.log");
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* ------------------------------------------------------- vtable patching */

#define MAX_VTBLS 16
#define MAX_SLOTS 256

static struct
{
    void **vtbl;
    void *orig[MAX_SLOTS];
} g_vtbls[MAX_VTBLS];
static int g_nvtbls;
static CRITICAL_SECTION g_patch_cs;

#define SLOT(vtbl_type, field) (offsetof(vtbl_type, field) / sizeof(void *))

static void *orig_fn(void *obj, size_t slot)
{
    void **vtbl = *(void ***)obj;
    int i;

    for (i = 0; i < g_nvtbls; i++)
        if (g_vtbls[i].vtbl == vtbl && g_vtbls[i].orig[slot])
            return g_vtbls[i].orig[slot];
    /* Not patched: the vtable entry is the real function. */
    return vtbl[slot];
}

static void patch_slot(void *obj, size_t slot, void *hook, const char *name)
{
    void **vtbl = *(void ***)obj;
    DWORD old;
    int i;

    EnterCriticalSection(&g_patch_cs);
    for (i = 0; i < g_nvtbls; i++)
        if (g_vtbls[i].vtbl == vtbl)
            break;
    if (i == g_nvtbls)
    {
        if (g_nvtbls == MAX_VTBLS)
        {
            log_msg("ERROR: too many vtables, cannot hook %s", name);
            LeaveCriticalSection(&g_patch_cs);
            return;
        }
        g_vtbls[g_nvtbls++].vtbl = vtbl;
    }
    if (vtbl[slot] != hook)
    {
        g_vtbls[i].orig[slot] = vtbl[slot];
        if (VirtualProtect(&vtbl[slot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old))
        {
            vtbl[slot] = hook;
            VirtualProtect(&vtbl[slot], sizeof(void *), old, &old);
            log_msg("hooked %s (vtbl %p)", name, vtbl);
        }
        else
        {
            g_vtbls[i].orig[slot] = NULL;
            log_msg("ERROR: VirtualProtect failed for %s", name);
        }
    }
    LeaveCriticalSection(&g_patch_cs);
}

/* Hooks call back into the runtime, which may itself dispatch through the
 * (patched) vtable. Only the outermost call is processed. */
static __thread int g_depth;

/* ------------------------------------------------------- buffer tracking */

typedef struct entry
{
    struct entry *next;
    ID3D11Buffer *buf;
    D3D11_BUFFER_DESC desc;
    BYTE *mirror;          /* CPU copy of the original contents, may be NULL */
    ID3D11Buffer *shadow;  /* padded copy, created with the first RGB32 view */
    DXGI_FORMAT fmt;       /* RGB32 format the shadow was expanded as */
} entry;

#define NBUCKETS 4096
static entry *g_buckets[NBUCKETS];
static CRITICAL_SECTION g_cs;

static struct
{
    LONG shadows, views, updates, maps, copies, untracked_copies, gpu_writable, unaligned;
    LONGLONG shadow_bytes, mirror_bytes;
} g_stats;

static unsigned bucket_of(void *p)
{
    return (unsigned)(((ULONG_PTR)p >> 4) * 2654435761u) % NBUCKETS;
}

static entry *find_entry(void *buf)
{
    entry *e;
    for (e = g_buckets[bucket_of(buf)]; e; e = e->next)
        if (e->buf == buf)
            return e;
    return NULL;
}

static void free_entry(entry *e)
{
    if (e->shadow)
        ID3D11Buffer_Release(e->shadow);
    if (e->mirror)
    {
        g_stats.mirror_bytes -= e->desc.ByteWidth;
        free(e->mirror);
    }
    free(e);
}

static void remove_entry(void *buf)
{
    entry **pe = &g_buckets[bucket_of(buf)];
    while (*pe)
    {
        if ((*pe)->buf == buf)
        {
            entry *e = *pe;
            *pe = e->next;
            free_entry(e);
            return;
        }
        pe = &(*pe)->next;
    }
}

static entry *add_entry(ID3D11Buffer *buf, const D3D11_BUFFER_DESC *desc)
{
    entry *e = calloc(1, sizeof(*e));
    unsigned b = bucket_of(buf);
    e->buf = buf;
    e->desc = *desc;
    e->next = g_buckets[b];
    g_buckets[b] = e;
    return e;
}

static BOOL ensure_mirror(entry *e)
{
    if (e->mirror)
        return TRUE;
    e->mirror = calloc(1, e->desc.ByteWidth);
    if (!e->mirror)
        return FALSE;
    g_stats.mirror_bytes += e->desc.ByteWidth;
    return TRUE;
}

static BOOL is_rgb32(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R32G32B32_FLOAT || f == DXGI_FORMAT_R32G32B32_UINT
        || f == DXGI_FORMAT_R32G32B32_SINT || f == DXGI_FORMAT_R32G32B32_TYPELESS;
}

static DXGI_FORMAT rgba32_of(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_UINT:  return DXGI_FORMAT_R32G32B32A32_UINT;
    case DXGI_FORMAT_R32G32B32_SINT:  return DXGI_FORMAT_R32G32B32A32_SINT;
    default:                          return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    }
}

/* Expand elements [first, first + count) from 12-byte src into 16-byte dst.
 * Missing source bytes (past the end of the buffer) read as zero. W is 1,
 * which is what a 3-component typed load returns. */
static void expand(const BYTE *src, UINT src_size, UINT first, UINT count, DXGI_FORMAT fmt, BYTE *dst)
{
    UINT w = fmt == DXGI_FORMAT_R32G32B32_FLOAT ? 0x3f800000u : 1u;
    UINT i;

    for (i = 0; i < count; i++)
    {
        UINT so = (first + i) * 12;
        UINT *d = (UINT *)(dst + i * 16);
        if (so + 12 <= src_size)
            memcpy(d, src + so, 12);
        else
        {
            memset(d, 0, 12);
            if (so < src_size)
                memcpy(d, src + so, src_size - so);
        }
        d[3] = w;
    }
}

static UINT elem_count(const entry *e)
{
    return (e->desc.ByteWidth + 11) / 12;
}

/* ------------------------------------------------------- device hooks */

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateBuffer)(ID3D11Device *, const D3D11_BUFFER_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Buffer **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSRV)(ID3D11Device *, ID3D11Resource *, const D3D11_SHADER_RESOURCE_VIEW_DESC *, ID3D11ShaderResourceView **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSRV1)(ID3D11Device3 *, ID3D11Resource *, const D3D11_SHADER_RESOURCE_VIEW_DESC1 *, ID3D11ShaderResourceView1 **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDeferred)(ID3D11Device *, UINT, ID3D11DeviceContext **);
typedef void (STDMETHODCALLTYPE *PFN_GetImmediate)(ID3D11Device *, ID3D11DeviceContext **);

static void hook_context(ID3D11DeviceContext *ctx);

static HRESULT STDMETHODCALLTYPE hk_CreateBuffer(ID3D11Device *This, const D3D11_BUFFER_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *init, ID3D11Buffer **out)
{
    PFN_CreateBuffer orig = orig_fn(This, SLOT(ID3D11DeviceVtbl, CreateBuffer));
    HRESULT hr;

    if (g_depth)
        return orig(This, desc, init, out);
    g_depth++;
    hr = orig(This, desc, init, out);
    if (SUCCEEDED(hr) && out && *out && desc)
    {
        EnterCriticalSection(&g_cs);
        /* A new buffer may reuse the address of a destroyed one. */
        remove_entry(*out);
        if ((desc->BindFlags & D3D11_BIND_SHADER_RESOURCE)
                && !(desc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)
                && desc->Usage != D3D11_USAGE_STAGING && desc->ByteWidth >= 12)
        {
            entry *e = add_entry(*out, desc);
            if (init && init->pSysMem && ensure_mirror(e))
                memcpy(e->mirror, init->pSysMem, desc->ByteWidth);
        }
        LeaveCriticalSection(&g_cs);
    }
    g_depth--;
    return hr;
}

/* Returns the shadow buffer for buf (creating it), or NULL. Called with g_cs held. */
static ID3D11Buffer *get_shadow(ID3D11Device *dev, ID3D11Buffer *buf, DXGI_FORMAT fmt)
{
    PFN_CreateBuffer create = orig_fn(dev, SLOT(ID3D11DeviceVtbl, CreateBuffer));
    D3D11_SUBRESOURCE_DATA init;
    D3D11_BUFFER_DESC sd;
    BYTE *data = NULL;
    entry *e = find_entry(buf);
    HRESULT hr;

    if (!e)
    {
        D3D11_BUFFER_DESC d;
        ID3D11Buffer_GetDesc(buf, &d);
        e = add_entry(buf, &d);
        log_msg("WARNING: RGB32 view on buffer %p created before hooks (size %u)", buf, d.ByteWidth);
    }
    if (e->shadow)
        return e->shadow;

    e->fmt = fmt;
    sd.ByteWidth = elem_count(e) * 16;
    sd.Usage = e->desc.Usage;
    sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.CPUAccessFlags = e->desc.Usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
    sd.MiscFlags = 0;
    sd.StructureByteStride = 0;

    if (e->mirror)
    {
        data = malloc(sd.ByteWidth);
        if (!data)
            return NULL;
        expand(e->mirror, e->desc.ByteWidth, 0, elem_count(e), fmt, data);
    }
    else if (sd.Usage == D3D11_USAGE_IMMUTABLE)
    {
        log_msg("ERROR: immutable buffer %p without data", buf);
        return NULL;
    }
    init.pSysMem = data;
    init.SysMemPitch = sd.ByteWidth;
    init.SysMemSlicePitch = sd.ByteWidth;

    hr = create(dev, &sd, data ? &init : NULL, &e->shadow);
    free(data);
    if (FAILED(hr))
    {
        log_msg("ERROR: shadow CreateBuffer failed %#lx (size %u usage %u)", hr, sd.ByteWidth, sd.Usage);
        e->shadow = NULL;
        return NULL;
    }
    /* Immutable data never changes again; dynamic buffers are expanded
     * straight from the mapped pointer. Only default buffers need the mirror
     * for partial updates. */
    if (e->desc.Usage != D3D11_USAGE_DEFAULT && e->mirror)
    {
        g_stats.mirror_bytes -= e->desc.ByteWidth;
        free(e->mirror);
        e->mirror = NULL;
    }
    if (e->desc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_RENDER_TARGET))
    {
        g_stats.gpu_writable++;
        log_msg("WARNING: buffer %p is GPU-writable (bind %#x); GPU writes will not reach the shadow", buf, e->desc.BindFlags);
    }
    g_stats.shadows++;
    g_stats.shadow_bytes += sd.ByteWidth;
    if (g_stats.shadows <= 50 || g_stats.shadows % 500 == 0)
        log_msg("shadow #%ld: buffer %p size %u usage %u bind %#x fmt %u data %s -> %u bytes (total %lld KB, mirrors %lld KB)",
                g_stats.shadows, buf, e->desc.ByteWidth, e->desc.Usage, e->desc.BindFlags, fmt,
                init.pSysMem ? "yes" : "no", sd.ByteWidth, g_stats.shadow_bytes / 1024, g_stats.mirror_bytes / 1024);
    return e->shadow;
}

static BOOL is_buffer(ID3D11Resource *res)
{
    D3D11_RESOURCE_DIMENSION dim;
    if (!res)
        return FALSE;
    ID3D11Resource_GetType(res, &dim);
    return dim == D3D11_RESOURCE_DIMENSION_BUFFER;
}

/* D3D11_SHADER_RESOURCE_VIEW_DESC and ..._DESC1 share the Format/ViewDimension/Buffer prefix. */
static BOOL wants_redirect(ID3D11Resource *res, DXGI_FORMAT fmt, D3D11_SRV_DIMENSION dim, UINT bufex_flags)
{
    if (!is_buffer(res))
        return FALSE;
    if (dim != D3D11_SRV_DIMENSION_BUFFER && dim != D3D11_SRV_DIMENSION_BUFFEREX)
        return FALSE;
    if (dim == D3D11_SRV_DIMENSION_BUFFEREX && (bufex_flags & D3D11_BUFFEREX_SRV_FLAG_RAW))
        return FALSE;
    return is_rgb32(fmt);
}

static void drop_untracked_view(ID3D11Resource *res)
{
    entry *e;
    /* A buffer viewed with another format is not face data; free its mirror. */
    if (!is_buffer(res))
        return;
    EnterCriticalSection(&g_cs);
    e = find_entry(res);
    if (e && !e->shadow)
        remove_entry(res);
    LeaveCriticalSection(&g_cs);
}

static HRESULT STDMETHODCALLTYPE hk_CreateSRV(ID3D11Device *This, ID3D11Resource *res,
        const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, ID3D11ShaderResourceView **out)
{
    PFN_CreateSRV orig = orig_fn(This, SLOT(ID3D11DeviceVtbl, CreateShaderResourceView));
    HRESULT hr;

    if (g_depth)
        return orig(This, res, desc, out);
    g_depth++;
    if (desc && wants_redirect(res, desc->Format, desc->ViewDimension, desc->BufferEx.Flags))
    {
        ID3D11Buffer *shadow;
        EnterCriticalSection(&g_cs);
        shadow = get_shadow(This, (ID3D11Buffer *)res, desc->Format);
        LeaveCriticalSection(&g_cs);
        if (shadow)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC nd = *desc;
            nd.Format = rgba32_of(desc->Format);
            hr = orig(This, (ID3D11Resource *)shadow, &nd, out);
            if (SUCCEEDED(hr))
                InterlockedIncrement(&g_stats.views);
            else
                log_msg("ERROR: redirected SRV failed %#lx", hr);
            g_depth--;
            return hr;
        }
    }
    else if (desc)
        drop_untracked_view(res);
    hr = orig(This, res, desc, out);
    g_depth--;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateSRV1(ID3D11Device3 *This, ID3D11Resource *res,
        const D3D11_SHADER_RESOURCE_VIEW_DESC1 *desc, ID3D11ShaderResourceView1 **out)
{
    PFN_CreateSRV1 orig = orig_fn(This, SLOT(ID3D11Device3Vtbl, CreateShaderResourceView1));
    HRESULT hr;

    if (g_depth)
        return orig(This, res, desc, out);
    g_depth++;
    if (desc && wants_redirect(res, desc->Format, desc->ViewDimension, desc->BufferEx.Flags))
    {
        ID3D11Buffer *shadow;
        EnterCriticalSection(&g_cs);
        shadow = get_shadow((ID3D11Device *)This, (ID3D11Buffer *)res, desc->Format);
        LeaveCriticalSection(&g_cs);
        if (shadow)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC1 nd = *desc;
            nd.Format = rgba32_of(desc->Format);
            hr = orig(This, (ID3D11Resource *)shadow, &nd, out);
            if (SUCCEEDED(hr))
                InterlockedIncrement(&g_stats.views);
            else
                log_msg("ERROR: redirected SRV1 failed %#lx", hr);
            g_depth--;
            return hr;
        }
    }
    else if (desc)
        drop_untracked_view(res);
    hr = orig(This, res, desc, out);
    g_depth--;
    return hr;
}

#define DEFERRED_HOOK(name, vtbl_type, dev_type, ctx_type) \
static HRESULT STDMETHODCALLTYPE hk_##name(dev_type *This, UINT flags, ctx_type **out) \
{ \
    HRESULT (STDMETHODCALLTYPE *orig)(dev_type *, UINT, ctx_type **) = orig_fn(This, SLOT(vtbl_type, name)); \
    HRESULT hr = orig(This, flags, out); \
    if (SUCCEEDED(hr) && out && *out) \
        hook_context((ID3D11DeviceContext *)*out); \
    return hr; \
}
DEFERRED_HOOK(CreateDeferredContext, ID3D11DeviceVtbl, ID3D11Device, ID3D11DeviceContext)
DEFERRED_HOOK(CreateDeferredContext1, ID3D11Device1Vtbl, ID3D11Device1, ID3D11DeviceContext1)
DEFERRED_HOOK(CreateDeferredContext2, ID3D11Device2Vtbl, ID3D11Device2, ID3D11DeviceContext2)
DEFERRED_HOOK(CreateDeferredContext3, ID3D11Device3Vtbl, ID3D11Device3, ID3D11DeviceContext3)

/* ------------------------------------------------------- context hooks */

typedef void (STDMETHODCALLTYPE *PFN_UpdateSubresource)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT);
typedef void (STDMETHODCALLTYPE *PFN_UpdateSubresource1)(ID3D11DeviceContext1 *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Map)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
typedef void (STDMETHODCALLTYPE *PFN_Unmap)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
typedef void (STDMETHODCALLTYPE *PFN_CopyResource)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
typedef void (STDMETHODCALLTYPE *PFN_CopySubresourceRegion)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT, ID3D11Resource *, UINT, const D3D11_BOX *);
typedef void (STDMETHODCALLTYPE *PFN_CopySubresourceRegion1)(ID3D11DeviceContext1 *, ID3D11Resource *, UINT, UINT, UINT, UINT, ID3D11Resource *, UINT, const D3D11_BOX *, UINT);

/* Re-expand bytes [lo, hi) of a default-usage buffer's mirror into its shadow. */
static void push_range(ID3D11DeviceContext *ctx, entry *e, UINT lo, UINT hi)
{
    PFN_UpdateSubresource update = orig_fn(ctx, SLOT(ID3D11DeviceContextVtbl, UpdateSubresource));
    UINT first = lo / 12, last = (hi + 11) / 12;
    D3D11_BOX box;
    BYTE *tmp;

    if (!e->shadow || !e->mirror || last <= first)
        return;
    tmp = malloc((last - first) * 16);
    if (!tmp)
        return;
    expand(e->mirror, e->desc.ByteWidth, first, last - first, e->fmt, tmp);
    box.left = first * 16;
    box.right = last * 16;
    box.top = 0;
    box.bottom = 1;
    box.front = 0;
    box.back = 1;
    update(ctx, (ID3D11Resource *)e->shadow, 0, &box, tmp, 0, 0);
    free(tmp);
}

static void after_update(ID3D11DeviceContext *ctx, ID3D11Resource *dst, const D3D11_BOX *box, const void *data)
{
    entry *e;
    UINT lo, hi;

    if (!data || !is_buffer(dst))
        return;
    EnterCriticalSection(&g_cs);
    e = find_entry(dst);
    if (e && ensure_mirror(e))
    {
        lo = box ? box->left : 0;
        hi = box ? box->right : e->desc.ByteWidth;
        if (hi > e->desc.ByteWidth)
            hi = e->desc.ByteWidth;
        if (lo < hi)
        {
            memcpy(e->mirror + lo, data, hi - lo);
            if (e->shadow)
            {
                push_range(ctx, e, lo, hi);
                InterlockedIncrement(&g_stats.updates);
            }
        }
    }
    LeaveCriticalSection(&g_cs);
}

static void STDMETHODCALLTYPE hk_UpdateSubresource(ID3D11DeviceContext *This, ID3D11Resource *dst, UINT sub,
        const D3D11_BOX *box, const void *data, UINT rp, UINT dp)
{
    PFN_UpdateSubresource orig = orig_fn(This, SLOT(ID3D11DeviceContextVtbl, UpdateSubresource));
    if (g_depth)
    {
        orig(This, dst, sub, box, data, rp, dp);
        return;
    }
    g_depth++;
    orig(This, dst, sub, box, data, rp, dp);
    after_update(This, dst, box, data);
    g_depth--;
}

static void STDMETHODCALLTYPE hk_UpdateSubresource1(ID3D11DeviceContext1 *This, ID3D11Resource *dst, UINT sub,
        const D3D11_BOX *box, const void *data, UINT rp, UINT dp, UINT flags)
{
    PFN_UpdateSubresource1 orig = orig_fn(This, SLOT(ID3D11DeviceContext1Vtbl, UpdateSubresource1));
    if (g_depth)
    {
        orig(This, dst, sub, box, data, rp, dp, flags);
        return;
    }
    g_depth++;
    orig(This, dst, sub, box, data, rp, dp, flags);
    /* D3D11_COPY_NO_OVERWRITE / DISCARD only affect synchronisation. */
    after_update((ID3D11DeviceContext *)This, dst, box, data);
    g_depth--;
}

/* Outstanding write maps of tracked buffers. */
#define MAX_MAPS 64
static struct
{
    ID3D11DeviceContext *ctx;
    ID3D11Resource *res;
    void *ptr;
} g_maps[MAX_MAPS];

static HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext *This, ID3D11Resource *res, UINT sub,
        D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *out)
{
    PFN_Map orig = orig_fn(This, SLOT(ID3D11DeviceContextVtbl, Map));
    HRESULT hr;
    int i;

    if (g_depth)
        return orig(This, res, sub, type, flags, out);
    g_depth++;
    hr = orig(This, res, sub, type, flags, out);
    if (SUCCEEDED(hr) && out && out->pData && type != D3D11_MAP_READ && is_buffer(res))
    {
        EnterCriticalSection(&g_cs);
        if (find_entry(res))
        {
            for (i = 0; i < MAX_MAPS; i++)
                if (!g_maps[i].res)
                {
                    g_maps[i].ctx = This;
                    g_maps[i].res = res;
                    g_maps[i].ptr = out->pData;
                    break;
                }
            if (i == MAX_MAPS)
                log_msg("ERROR: too many outstanding maps");
        }
        LeaveCriticalSection(&g_cs);
    }
    g_depth--;
    return hr;
}

static void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext *This, ID3D11Resource *res, UINT sub)
{
    PFN_Unmap orig = orig_fn(This, SLOT(ID3D11DeviceContextVtbl, Unmap));
    PFN_Map map = orig_fn(This, SLOT(ID3D11DeviceContextVtbl, Map));
    D3D11_MAPPED_SUBRESOURCE ms;
    void *ptr = NULL;
    entry *e;
    int i;

    if (g_depth)
    {
        orig(This, res, sub);
        return;
    }
    g_depth++;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < MAX_MAPS; i++)
        if (g_maps[i].res == res && g_maps[i].ctx == This)
        {
            ptr = g_maps[i].ptr;
            g_maps[i].res = NULL;
            break;
        }
    if (ptr && (e = find_entry(res)))
    {
        if (e->shadow && e->desc.Usage == D3D11_USAGE_DYNAMIC)
        {
            /* Read the app's data before the real Unmap invalidates ptr. */
            if (SUCCEEDED(map(This, (ID3D11Resource *)e->shadow, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
            {
                expand(ptr, e->desc.ByteWidth, 0, elem_count(e), e->fmt, ms.pData);
                orig(This, (ID3D11Resource *)e->shadow, 0);
                InterlockedIncrement(&g_stats.maps);
            }
        }
        else if (ensure_mirror(e))
        {
            memcpy(e->mirror, ptr, e->desc.ByteWidth);
            if (e->shadow)
            {
                push_range(This, e, 0, e->desc.ByteWidth);
                InterlockedIncrement(&g_stats.maps);
            }
        }
    }
    LeaveCriticalSection(&g_cs);
    orig(This, res, sub);
    g_depth--;
}

/* Mirror a copy between buffers into their shadows. Called after the real copy. */
static void after_copy(ID3D11DeviceContext *ctx, ID3D11Resource *dst, UINT dx, ID3D11Resource *src, const D3D11_BOX *box)
{
    PFN_CopySubresourceRegion copy = orig_fn(ctx, SLOT(ID3D11DeviceContextVtbl, CopySubresourceRegion));
    entry *de, *se;
    UINT lo, hi;

    if (!is_buffer(dst))
        return;
    EnterCriticalSection(&g_cs);
    de = find_entry(dst);
    if (de && de->shadow)
    {
        se = is_buffer(src) ? find_entry(src) : NULL;
        lo = box ? box->left : 0;
        hi = box ? box->right : de->desc.ByteWidth;
        if (se && se->mirror && de->mirror && hi <= se->desc.ByteWidth && dx + (hi - lo) <= de->desc.ByteWidth)
        {
            /* Both sides known on the CPU: copy the mirror and re-expand. */
            memmove(de->mirror + dx, se->mirror + lo, hi - lo);
            push_range(ctx, de, dx, dx + (hi - lo));
            InterlockedIncrement(&g_stats.copies);
        }
        else if (se && se->shadow && lo % 12 == 0 && hi % 12 == 0 && dx % 12 == 0)
        {
            /* Element-aligned shadow-to-shadow copy on the GPU. */
            D3D11_BOX sb = { lo / 12 * 16, 0, 0, hi / 12 * 16, 1, 1 };
            copy(ctx, (ID3D11Resource *)de->shadow, 0, dx / 12 * 16, 0, 0, (ID3D11Resource *)se->shadow, 0, &sb);
            if (de->mirror && se->mirror)
                memmove(de->mirror + dx, se->mirror + lo, hi - lo);
            InterlockedIncrement(&g_stats.copies);
        }
        else
        {
            LONG n = InterlockedIncrement(&g_stats.untracked_copies);
            if (n <= 20)
                log_msg("WARNING: copy into shadowed buffer %p from untracked %p (offset %u, range %u-%u) not mirrored",
                        dst, src, dx, lo, hi);
        }
    }
    LeaveCriticalSection(&g_cs);
}

static void STDMETHODCALLTYPE hk_CopyResource(ID3D11DeviceContext *This, ID3D11Resource *dst, ID3D11Resource *src)
{
    PFN_CopyResource orig = orig_fn(This, SLOT(ID3D11DeviceContextVtbl, CopyResource));
    if (g_depth)
    {
        orig(This, dst, src);
        return;
    }
    g_depth++;
    orig(This, dst, src);
    after_copy(This, dst, 0, src, NULL);
    g_depth--;
}

static void STDMETHODCALLTYPE hk_CopySubresourceRegion(ID3D11DeviceContext *This, ID3D11Resource *dst, UINT dsub,
        UINT x, UINT y, UINT z, ID3D11Resource *src, UINT ssub, const D3D11_BOX *box)
{
    PFN_CopySubresourceRegion orig = orig_fn(This, SLOT(ID3D11DeviceContextVtbl, CopySubresourceRegion));
    if (g_depth)
    {
        orig(This, dst, dsub, x, y, z, src, ssub, box);
        return;
    }
    g_depth++;
    orig(This, dst, dsub, x, y, z, src, ssub, box);
    after_copy(This, dst, x, src, box);
    g_depth--;
}

static void STDMETHODCALLTYPE hk_CopySubresourceRegion1(ID3D11DeviceContext1 *This, ID3D11Resource *dst, UINT dsub,
        UINT x, UINT y, UINT z, ID3D11Resource *src, UINT ssub, const D3D11_BOX *box, UINT flags)
{
    PFN_CopySubresourceRegion1 orig = orig_fn(This, SLOT(ID3D11DeviceContext1Vtbl, CopySubresourceRegion1));
    if (g_depth)
    {
        orig(This, dst, dsub, x, y, z, src, ssub, box, flags);
        return;
    }
    g_depth++;
    orig(This, dst, dsub, x, y, z, src, ssub, box, flags);
    after_copy((ID3D11DeviceContext *)This, dst, x, src, box);
    g_depth--;
}

static void hook_context(ID3D11DeviceContext *ctx)
{
    ID3D11DeviceContext1 *ctx1;

    patch_slot(ctx, SLOT(ID3D11DeviceContextVtbl, UpdateSubresource), hk_UpdateSubresource, "UpdateSubresource");
    patch_slot(ctx, SLOT(ID3D11DeviceContextVtbl, Map), hk_Map, "Map");
    patch_slot(ctx, SLOT(ID3D11DeviceContextVtbl, Unmap), hk_Unmap, "Unmap");
    patch_slot(ctx, SLOT(ID3D11DeviceContextVtbl, CopyResource), hk_CopyResource, "CopyResource");
    patch_slot(ctx, SLOT(ID3D11DeviceContextVtbl, CopySubresourceRegion), hk_CopySubresourceRegion, "CopySubresourceRegion");
    if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void **)&ctx1)))
    {
        patch_slot(ctx1, SLOT(ID3D11DeviceContext1Vtbl, UpdateSubresource1), hk_UpdateSubresource1, "UpdateSubresource1");
        patch_slot(ctx1, SLOT(ID3D11DeviceContext1Vtbl, CopySubresourceRegion1), hk_CopySubresourceRegion1, "CopySubresourceRegion1");
        ID3D11DeviceContext1_Release(ctx1);
    }
}

static void hook_device(ID3D11Device *dev)
{
    ID3D11DeviceContext *ctx;
    ID3D11Device3 *dev3;
    ID3D11Device2 *dev2;
    ID3D11Device1 *dev1;

    patch_slot(dev, SLOT(ID3D11DeviceVtbl, CreateBuffer), hk_CreateBuffer, "CreateBuffer");
    patch_slot(dev, SLOT(ID3D11DeviceVtbl, CreateShaderResourceView), hk_CreateSRV, "CreateShaderResourceView");
    patch_slot(dev, SLOT(ID3D11DeviceVtbl, CreateDeferredContext), hk_CreateDeferredContext, "CreateDeferredContext");
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **)&dev1)))
    {
        patch_slot(dev1, SLOT(ID3D11Device1Vtbl, CreateDeferredContext1), hk_CreateDeferredContext1, "CreateDeferredContext1");
        ID3D11Device1_Release(dev1);
    }
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device2, (void **)&dev2)))
    {
        patch_slot(dev2, SLOT(ID3D11Device2Vtbl, CreateDeferredContext2), hk_CreateDeferredContext2, "CreateDeferredContext2");
        ID3D11Device2_Release(dev2);
    }
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device3, (void **)&dev3)))
    {
        patch_slot(dev3, SLOT(ID3D11Device3Vtbl, CreateShaderResourceView1), hk_CreateSRV1, "CreateShaderResourceView1");
        patch_slot(dev3, SLOT(ID3D11Device3Vtbl, CreateDeferredContext3), hk_CreateDeferredContext3, "CreateDeferredContext3");
        ID3D11Device3_Release(dev3);
    }
    ID3D11Device_GetImmediateContext(dev, &ctx);
    if (ctx)
    {
        hook_context(ctx);
        ID3D11DeviceContext_Release(ctx);
    }
}

/* ------------------------------------------------------- movie decoder */

DEFINE_GUID(CLSID_H264DecoderMFT, 0x62ce7e72, 0x4c71, 0x4d20, 0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d);

static BOOL is_subtype(IMFMediaType *type, const GUID *subtype)
{
    GUID g;
    return SUCCEEDED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &g)) && IsEqualGUID(&g, subtype);
}

static BOOL g_logged_nv12;

/* Offers NV12 as type 0, followed by the decoder's own list. NV12 has the same
 * sample size and default stride as the YV12 type it is cloned from. */
static HRESULT STDMETHODCALLTYPE hk_GetOutputAvailableType(IMFTransform *This, DWORD id, DWORD index,
        IMFMediaType **type)
{
    HRESULT (STDMETHODCALLTYPE *real)(IMFTransform *, DWORD, DWORD, IMFMediaType **) =
            orig_fn(This, SLOT(IMFTransformVtbl, GetOutputAvailableType));
    IMFMediaType *first, *nv12;
    HRESULT hr;

    if (!type || FAILED(real(This, id, 0, &first)))
        return real(This, id, index, type);
    if (is_subtype(first, &MFVideoFormat_NV12))
    {
        if (!g_logged_nv12)
            log_msg("H.264 decoder already offers NV12 first, leaving it alone");
        g_logged_nv12 = TRUE;
        IMFMediaType_Release(first);
        return real(This, id, index, type);
    }
    if (!g_logged_nv12)
        log_msg("H.264 decoder does not offer NV12, adding it as type 0");
    g_logged_nv12 = TRUE;
    if (index)
    {
        IMFMediaType_Release(first);
        return real(This, id, index - 1, type);
    }
    if (FAILED(hr = MFCreateMediaType(&nv12)))
    {
        *type = first;
        return S_OK;
    }
    IMFMediaType_CopyAllItems(first, (IMFAttributes *)nv12);
    IMFMediaType_SetGUID(nv12, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    IMFMediaType_Release(first);
    *type = nv12;
    return S_OK;
}

/* Windows' H.264 decoder hands out frames in display order even when the input samples carry
 * no timestamps, as MK11's don't. CrossOver's hands them out in decode order, so B-frames play
 * out of order and the movie looks choppy. The hooks read each frame's picture order count
 * (POC) from its slice header and hold back a few decoded frames to return them in POC order.
 * Streams the parser does not understand are passed through untouched. */

#define REORDER_DEPTH 3
#define MAX_DECODERS 4
#define MAX_PENDING 64

typedef struct
{
    const BYTE *p;
    size_t len, bit;
} bitreader;

static UINT read_bits(bitreader *b, int n)
{
    UINT v = 0;
    while (n--)
    {
        v <<= 1;
        if (b->bit < b->len * 8)
            v |= (b->p[b->bit / 8] >> (7 - b->bit % 8)) & 1;
        b->bit++;
    }
    return v;
}

static UINT read_ue(bitreader *b)
{
    int zeros = 0;
    while (!read_bits(b, 1) && zeros < 32)
        zeros++;
    return (1u << zeros) - 1 + read_bits(b, zeros);
}

/* Copies up to max bytes of a NAL unit payload with emulation prevention bytes removed. */
static size_t unescape(const BYTE *src, size_t len, BYTE *dst, size_t max)
{
    size_t i, n = 0, zeros = 0;
    for (i = 0; i < len && n < max; i++)
    {
        if (zeros >= 2 && src[i] == 3)
        {
            zeros = 0;
            continue;
        }
        zeros = src[i] ? 0 : zeros + 1;
        dst[n++] = src[i];
    }
    return n;
}

typedef struct
{
    IMFTransform *t;
    BOOL disabled, have_sps;
    int log2_frame_num, log2_poc_lsb;
    int prev_msb, prev_lsb;
    LONGLONG epoch;
    LONGLONG pending[MAX_PENDING];
    int head, count;
    struct { LONGLONG key; BYTE *data; DWORD len; } held[REORDER_DEPTH + 1];
    int nheld;
    BOOL draining;
    LONG frames, moved;
} decoder_state;

static decoder_state g_dec[MAX_DECODERS];
static CRITICAL_SECTION g_dec_cs;

static decoder_state *get_decoder(IMFTransform *t, BOOL create)
{
    int i;
    for (i = 0; i < MAX_DECODERS; i++)
        if (g_dec[i].t == t)
            return &g_dec[i];
    if (!create)
        return NULL;
    for (i = 0; i < MAX_DECODERS; i++)
        if (!g_dec[i].t)
        {
            memset(&g_dec[i], 0, sizeof(g_dec[i]));
            g_dec[i].t = t;
            return &g_dec[i];
        }
    return NULL;
}

static void reset_decoder(decoder_state *d)
{
    int i;
    if (d->frames)
        log_msg("movie: %ld frames, %ld returned in a different order than decoded", d->frames, d->moved);
    for (i = 0; i < d->nheld; i++)
        free(d->held[i].data);
    d->nheld = d->head = d->count = 0;
    d->prev_msb = d->prev_lsb = 0;
    d->frames = d->moved = 0;
    d->draining = FALSE;
}

static BOOL parse_sps(decoder_state *d, const BYTE *nal, size_t len)
{
    BYTE buf[64];
    bitreader b = {buf, unescape(nal + 1, len - 1, buf, sizeof(buf)), 0};
    UINT profile = read_bits(&b, 8);

    read_bits(&b, 16); /* constraint flags, level */
    read_ue(&b);       /* seq_parameter_set_id */
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83
            || profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139
            || profile == 134 || profile == 135)
    {
        if (read_ue(&b) == 3) /* chroma_format_idc */
            return FALSE;
        read_ue(&b);              /* bit_depth_luma_minus8 */
        read_ue(&b);              /* bit_depth_chroma_minus8 */
        read_bits(&b, 1);         /* qpprime_y_zero_transform_bypass_flag */
        if (read_bits(&b, 1))     /* seq_scaling_matrix_present_flag */
            return FALSE;
    }
    d->log2_frame_num = read_ue(&b) + 4;
    if (read_ue(&b) != 0) /* pic_order_cnt_type */
        return FALSE;
    d->log2_poc_lsb = read_ue(&b) + 4;
    read_ue(&b);      /* max_num_ref_frames */
    read_bits(&b, 1); /* gaps_in_frame_num_value_allowed_flag */
    read_ue(&b);      /* pic_width_in_mbs_minus1 */
    read_ue(&b);      /* pic_height_in_map_units_minus1 */
    if (!read_bits(&b, 1)) /* frame_mbs_only_flag */
        return FALSE;
    return b.bit <= b.len * 8 && d->log2_frame_num <= 16 && d->log2_poc_lsb <= 16;
}

/* Returns the display order key of the frame in an Annex B access unit, or -1. */
static LONGLONG frame_key(decoder_state *d, const BYTE *p, size_t len)
{
    size_t i = 0;

    while (i + 3 < len)
    {
        size_t start, end;
        int type, ref;

        if (p[i] || p[i + 1] || p[i + 2] != 1)
        {
            i++;
            continue;
        }
        start = i + 3;
        for (end = start; end + 2 < len && (p[end] || p[end + 1] || p[end + 2] > 1); end++)
            ;
        if (end + 2 >= len)
            end = len;
        i = end;
        type = p[start] & 0x1f;
        ref = (p[start] >> 5) & 3;
        if (type == 7)
        {
            if (!(d->have_sps = parse_sps(d, p + start, end - start)))
                return -1;
        }
        else if ((type == 1 || type == 5) && d->have_sps)
        {
            BYTE buf[64];
            bitreader b = {buf, unescape(p + start + 1, end - start - 1, buf, sizeof(buf)), 0};
            int max_lsb = 1 << d->log2_poc_lsb, lsb, msb;

            read_ue(&b); /* first_mb_in_slice */
            read_ue(&b); /* slice_type */
            read_ue(&b); /* pic_parameter_set_id */
            read_bits(&b, d->log2_frame_num);
            if (type == 5)
            {
                read_ue(&b); /* idr_pic_id */
                d->prev_msb = d->prev_lsb = 0;
                d->epoch++;
            }
            lsb = read_bits(&b, d->log2_poc_lsb);
            if (b.bit > b.len * 8)
                return -1;
            if (lsb < d->prev_lsb && d->prev_lsb - lsb >= max_lsb / 2)
                msb = d->prev_msb + max_lsb;
            else if (lsb > d->prev_lsb && lsb - d->prev_lsb > max_lsb / 2)
                msb = d->prev_msb - max_lsb;
            else
                msb = d->prev_msb;
            if (ref)
            {
                d->prev_msb = msb;
                d->prev_lsb = lsb;
            }
            return (d->epoch << 32) + (msb + lsb) + 0x40000000;
        }
    }
    return -1;
}

static HRESULT STDMETHODCALLTYPE hk_ProcessInput(IMFTransform *This, DWORD id, IMFSample *sample, DWORD flags)
{
    HRESULT (STDMETHODCALLTYPE *real)(IMFTransform *, DWORD, IMFSample *, DWORD) =
            orig_fn(This, SLOT(IMFTransformVtbl, ProcessInput));
    decoder_state *d;
    LONGLONG key = -1;
    IMFMediaBuffer *buf;
    HRESULT hr;

    EnterCriticalSection(&g_dec_cs);
    d = get_decoder(This, TRUE);
    if (d && !d->disabled && sample && SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buf)))
    {
        BYTE *p;
        DWORD len;
        decoder_state saved = *d;
        if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &len)))
        {
            key = frame_key(d, p, len);
            IMFMediaBuffer_Unlock(buf);
        }
        IMFMediaBuffer_Release(buf);
        hr = real(This, id, sample, flags);
        if (FAILED(hr))
        {
            /* Not consumed; the game will send the same frame again. */
            d->have_sps = saved.have_sps;
            d->prev_msb = saved.prev_msb;
            d->prev_lsb = saved.prev_lsb;
            d->epoch = saved.epoch;
        }
        else if (key < 0 || d->count == MAX_PENDING)
        {
            log_msg("movie: cannot read frame order, leaving frames as decoded");
            d->disabled = TRUE;
        }
        else
        {
            d->pending[(d->head + d->count++) % MAX_PENDING] = key;
            d->draining = FALSE;
        }
        LeaveCriticalSection(&g_dec_cs);
        return hr;
    }
    LeaveCriticalSection(&g_dec_cs);
    return real(This, id, sample, flags);
}

static BOOL copy_sample(IMFSample *sample, BYTE **data, DWORD *len, BOOL to_sample)
{
    IMFMediaBuffer *buf;
    BYTE *p;
    DWORD max, cur;
    BOOL ok = FALSE;

    if (FAILED(IMFSample_GetBufferByIndex(sample, 0, &buf)))
        return FALSE;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, &max, &cur)))
    {
        if (to_sample && *len <= max)
        {
            memcpy(p, *data, *len);
            IMFMediaBuffer_SetCurrentLength(buf, *len);
            ok = TRUE;
        }
        else if (!to_sample && (*data = malloc(cur)))
        {
            memcpy(*data, p, cur);
            *len = cur;
            ok = TRUE;
        }
        IMFMediaBuffer_Unlock(buf);
    }
    IMFMediaBuffer_Release(buf);
    return ok;
}

/* Moves the held frame that comes first in display order into the game's sample. */
static HRESULT emit_frame(decoder_state *d, MFT_OUTPUT_DATA_BUFFER *out, DWORD *status)
{
    int i, best = 0;
    BOOL ok;

    for (i = 1; i < d->nheld; i++)
        if (d->held[i].key < d->held[best].key)
            best = i;
    ok = copy_sample(out->pSample, &d->held[best].data, &d->held[best].len, TRUE);
    if (best)
        d->moved++;
    free(d->held[best].data);
    /* Keep the rest oldest first, so index 0 is the next frame in decode order. */
    memmove(&d->held[best], &d->held[best + 1], (--d->nheld - best) * sizeof(d->held[0]));
    out->dwStatus = 0;
    if (status)
        *status = 0;
    return ok ? S_OK : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE hk_ProcessOutput(IMFTransform *This, DWORD flags, DWORD count,
        MFT_OUTPUT_DATA_BUFFER *bufs, DWORD *status)
{
    HRESULT (STDMETHODCALLTYPE *real)(IMFTransform *, DWORD, DWORD, MFT_OUTPUT_DATA_BUFFER *, DWORD *) =
            orig_fn(This, SLOT(IMFTransformVtbl, ProcessOutput));
    decoder_state *d;
    HRESULT hr;

    EnterCriticalSection(&g_dec_cs);
    d = get_decoder(This, FALSE);
    if (!d || d->disabled || count != 1 || !bufs || !bufs[0].pSample)
    {
        LeaveCriticalSection(&g_dec_cs);
        return real(This, flags, count, bufs, status);
    }
    hr = real(This, flags, count, bufs, status);
    if (hr == S_OK)
    {
        BYTE *data;
        DWORD len;
        if (!d->count || !copy_sample(bufs[0].pSample, &data, &len, FALSE))
        {
            /* Output does not line up with input; stop reordering and return held frames first. */
            log_msg("movie: decoder output does not match input, leaving frames as decoded");
            d->disabled = TRUE;
            LeaveCriticalSection(&g_dec_cs);
            return hr;
        }
        d->held[d->nheld].key = d->pending[d->head];
        d->held[d->nheld].data = data;
        d->held[d->nheld].len = len;
        d->nheld++;
        d->head = (d->head + 1) % MAX_PENDING;
        d->count--;
        d->frames++;
        if (d->nheld > REORDER_DEPTH)
            hr = emit_frame(d, &bufs[0], status);
        else
        {
            bufs[0].dwStatus = 0;
            hr = MF_E_TRANSFORM_NEED_MORE_INPUT;
        }
    }
    else if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT && d->draining && d->nheld)
        hr = emit_frame(d, &bufs[0], status);
    LeaveCriticalSection(&g_dec_cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_ProcessMessage(IMFTransform *This, MFT_MESSAGE_TYPE msg, ULONG_PTR param)
{
    HRESULT (STDMETHODCALLTYPE *real)(IMFTransform *, MFT_MESSAGE_TYPE, ULONG_PTR) =
            orig_fn(This, SLOT(IMFTransformVtbl, ProcessMessage));
    decoder_state *d;

    EnterCriticalSection(&g_dec_cs);
    if ((d = get_decoder(This, FALSE)))
    {
        if (msg == MFT_MESSAGE_COMMAND_FLUSH || msg == MFT_MESSAGE_NOTIFY_START_OF_STREAM
                || msg == MFT_MESSAGE_NOTIFY_BEGIN_STREAMING)
        {
            reset_decoder(d);
            d->disabled = FALSE;
        }
        else if (msg == MFT_MESSAGE_COMMAND_DRAIN)
            d->draining = TRUE;
    }
    LeaveCriticalSection(&g_dec_cs);
    return real(This, msg, param);
}

typedef HRESULT (WINAPI *PFN_CoCreateInstance)(REFCLSID, IUnknown *, DWORD, REFIID, void **);
static PFN_CoCreateInstance g_real_cocreate;

static HRESULT WINAPI hk_CoCreateInstance(REFCLSID clsid, IUnknown *outer, DWORD ctx, REFIID iid, void **out)
{
    HRESULT hr = g_real_cocreate(clsid, outer, ctx, iid, out);
    IMFTransform *t;

    if (FAILED(hr) || !out || !*out || !IsEqualGUID(clsid, &CLSID_H264DecoderMFT))
        return hr;
    if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*out, &IID_IMFTransform, (void **)&t)))
    {
        patch_slot(t, SLOT(IMFTransformVtbl, GetOutputAvailableType), hk_GetOutputAvailableType,
                "IMFTransform::GetOutputAvailableType");
        patch_slot(t, SLOT(IMFTransformVtbl, ProcessInput), hk_ProcessInput, "IMFTransform::ProcessInput");
        patch_slot(t, SLOT(IMFTransformVtbl, ProcessOutput), hk_ProcessOutput, "IMFTransform::ProcessOutput");
        patch_slot(t, SLOT(IMFTransformVtbl, ProcessMessage), hk_ProcessMessage, "IMFTransform::ProcessMessage");
        IMFTransform_Release(t);
    }
    return hr;
}

/* ------------------------------------------------------- entry points */

typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
static PFN_D3D11CreateDevice g_real_create;

static HRESULT WINAPI hk_D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
        const D3D_FEATURE_LEVEL *levels, UINT nlevels, UINT sdk, ID3D11Device **dev, D3D_FEATURE_LEVEL *level,
        ID3D11DeviceContext **ctx)
{
    ID3D11Device *d = NULL;
    HRESULT hr = g_real_create(adapter, type, sw, flags, levels, nlevels, sdk, dev ? dev : &d, level, ctx);

    log_msg("D3D11CreateDevice(flags %#x) -> %#lx", flags, hr);
    if (SUCCEEDED(hr) && dev && *dev)
        hook_device(*dev);
    if (d)
        ID3D11Device_Release(d);
    return hr;
}

static BOOL patch_iat(HMODULE mod, const char *dll, const char *func, void *hook, void **orig)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;

    if (!dir->VirtualAddress)
        return FALSE;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++)
    {
        IMAGE_THUNK_DATA *names, *addrs;
        if (_stricmp((char *)(base + imp->Name), dll))
            continue;
        names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++)
        {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, func))
                continue;
            if (!VirtualProtect(&addrs->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
                return FALSE;
            *orig = (void *)addrs->u1.Function;
            addrs->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&addrs->u1.Function, sizeof(void *), old, &old);
            return TRUE;
        }
    }
    return FALSE;
}

static DWORD WINAPI stats_thread(void *arg)
{
    for (;;)
    {
        Sleep(30000);
        log_msg("stats: shadows %ld (%lld KB), redirected views %ld, update syncs %ld, map syncs %ld, copy syncs %ld, "
                "untracked copies %ld, gpu-writable %ld, mirrors %lld KB",
                g_stats.shadows, g_stats.shadow_bytes / 1024, g_stats.views, g_stats.updates, g_stats.maps,
                g_stats.copies, g_stats.untracked_copies, g_stats.gpu_writable, g_stats.mirror_bytes / 1024);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(inst);
    open_log(inst);
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_patch_cs);
    InitializeCriticalSection(&g_dec_cs);
    log_msg("mk11 rgb32 fix loaded");
    if (patch_iat(GetModuleHandleA(NULL), "d3d11.dll", "D3D11CreateDevice", hk_D3D11CreateDevice, (void **)&g_real_create))
        log_msg("hooked D3D11CreateDevice import");
    else
        log_msg("ERROR: D3D11CreateDevice import not found");
    if (patch_iat(GetModuleHandleA(NULL), "ole32.dll", "CoCreateInstance", hk_CoCreateInstance, (void **)&g_real_cocreate))
        log_msg("hooked CoCreateInstance import");
    else
        log_msg("ERROR: CoCreateInstance import not found");
    CloseHandle(CreateThread(NULL, 0, stats_thread, NULL, 0, NULL));
    return TRUE;
}
