"""
MsgPacket Python bindings setup
"""
from setuptools import setup, find_packages

setup(
    name="msgpacket",
    version="1.1.0",
    description="MsgPacket protocol Python bindings (FFI, zero-copy)",
    readme="README.md",
    python_requires=">=3.6",
    packages=find_packages(where="."),
    package_data={
        "msgpacket": ["py.typed", "*.dll", "*.so", "x64/*.dll", "Lnx64/*.so", "MacOS64/*.dylib"],
    },
    classifiers=[
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.6",
        "Programming Language :: Python :: 3.7",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "License :: OSI Approved :: MIT",
    ],
)