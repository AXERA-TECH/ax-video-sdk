# 平台 wheel 但不绑 Python 版本:py3-none-linux_{x86_64,aarch64}。
# so 是 ctypes 加载的普通 C 库,与 CPython ABI 无关。
import os
from setuptools import setup

try:
    from setuptools.command.bdist_wheel import bdist_wheel
except ImportError:  # older setuptools
    from wheel.bdist_wheel import bdist_wheel


class BdistWheel(bdist_wheel):
    def finalize_options(self):
        super().finalize_options()
        self.root_is_pure = False

    def get_tag(self):
        return "py3", "none", os.environ.get("PYAXV_PLAT", "linux_x86_64")


setup(cmdclass={"bdist_wheel": BdistWheel})
