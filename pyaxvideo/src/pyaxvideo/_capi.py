# ctypes 声明与后端 so 加载。
# 一个 C ABI、每后端一个 so(libaxvideo_capi_axcl.so / libaxvideo_capi_ax650.so),
# 运行时只加载被选中的那个——另一个后端的运行库不存在也不会报错。
import ctypes
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
_NATIVE = os.path.join(_HERE, "_native")


class FrameInfo(ctypes.Structure):
    _fields_ = [
        ("format", ctypes.c_int32),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("plane_count", ctypes.c_uint32),
        ("strides", ctypes.c_size_t * 3),
        ("plane_sizes", ctypes.c_size_t * 3),
        ("byte_size", ctypes.c_size_t),
        ("phys_addr", ctypes.c_uint64 * 3),
        ("virt_addr", ctypes.c_uint64 * 3),
        ("device_id", ctypes.c_int32),
    ]


class ReaderOpts(ctypes.Structure):
    _fields_ = [
        ("latest_only", ctypes.c_int),
        ("realtime", ctypes.c_int),
        ("loop", ctypes.c_int),
        ("queue_depth", ctypes.c_int),
    ]


class WriterOpts(ctypes.Structure):
    _fields_ = [
        ("codec", ctypes.c_int32),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("fps", ctypes.c_double),
        ("bitrate_kbps", ctypes.c_uint32),
        ("gop", ctypes.c_uint32),
    ]


def _declare(lib):
    c = ctypes
    P = c.c_void_p
    sigs = {
        "axv_init": ([c.c_int], c.c_int),
        "axv_deinit": ([], None),
        "axv_backend": ([], c.c_char_p),
        "axv_version": ([], c.c_char_p),
        "axv_last_error": ([], c.c_char_p),
        "axv_frame_get_info": ([P, c.POINTER(FrameInfo)], c.c_int),
        "axv_frame_release": ([P], None),
        "axv_frame_clone": ([P], P),
        "axv_frame_to_host": ([P, P, c.c_size_t], c.c_int),
        "axv_frame_from_host": ([c.c_int32, c.c_uint32, c.c_uint32, P, c.c_size_t, c.c_size_t], P),
        "axv_frame_convert": ([P, c.c_int32, c.c_uint32, c.c_uint32, c.c_int, c.c_uint32], P),
        "axv_frame_crop": ([P, c.c_int32, c.c_int32, c.c_uint32, c.c_uint32,
                            c.c_int32, c.c_uint32, c.c_uint32], P),
        "axv_frame_encode_jpeg": ([P, c.c_uint32, c.POINTER(c.POINTER(c.c_uint8)),
                                   c.POINTER(c.c_size_t)], c.c_int),
        "axv_buffer_free": ([c.POINTER(c.c_uint8)], None),
        "axv_decode_jpeg": ([P, c.c_size_t], P),
        "axv_reader_open": ([c.c_char_p, c.POINTER(ReaderOpts)], P),
        "axv_reader_next": ([P, c.POINTER(P), c.c_int], c.c_int),
        "axv_reader_get_info": ([P, c.POINTER(c.c_int32), c.POINTER(c.c_uint32),
                                 c.POINTER(c.c_uint32), c.POINTER(c.c_double)], c.c_int),
        "axv_reader_close": ([P], None),
        "axv_writer_open": ([c.c_char_p, c.POINTER(WriterOpts)], P),
        "axv_writer_write": ([P, P], c.c_int),
        "axv_writer_close": ([P], None),
    }
    for name, (argtypes, restype) in sigs.items():
        fn = getattr(lib, name)
        fn.argtypes = argtypes
        fn.restype = restype
    return lib


def load(backend):
    path = os.path.join(_NATIVE, "libaxvideo_capi_%s.so" % backend)
    if not os.path.exists(path):
        raise RuntimeError(
            "backend %r not bundled in this wheel (missing %s)" % (backend, path))
    # 板端 MSP 库位置(/soc/lib)由 so 里烧的 DT_RPATH 解决;
    # RTLD_LAZY:个别 MSP 库存在未声明依赖的悬空函数符号(如 libax_venc 的 exif_*),
    # 与官方 app 相同的惰性绑定语义,不走到那条路径就不会解析。
    return _declare(ctypes.CDLL(path, mode=os.RTLD_LAZY | os.RTLD_LOCAL))
