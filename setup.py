#!/usr/bin/env python
# USE_SYCL - to use SYCL
# USE_USM - to use USM
from __future__ import print_function

from subprocess import check_call
from setuptools import setup, Extension, find_packages, distutils
import setuptools.command.build_ext
import setuptools.command.install
from distutils.spawn import find_executable
from sysconfig import get_paths

import distutils.command.clean
import git
import glob
import inspect
import multiprocessing
import multiprocessing.pool
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys

try:
    import torch
    from torch.utils.cpp_extension import include_paths, library_paths
except ImportError as e:
    print('Unable to import torch. Error:')
    print('\t', e)
    print('You need to install pytorch first.')
    sys.exit(1)

base_dir = os.path.dirname(os.path.abspath(__file__))

def _get_complier():
    if not os.getenv("DPCPP_ROOT") is None:
        # dpcpp build
        return "clang", "clang++"
    else:
        return "gcc", "compute++"

def _check_env_flag(name, default=''):
  return os.getenv(name, default).upper() in ['ON', '1', 'YES', 'TRUE', 'Y']


def _get_env_backend():
  env_backend_var_name = 'IPEX_BACKEND'
  env_backend_options = ['cpu', 'gpu']
  env_backend_val = os.getenv(env_backend_var_name)
  if env_backend_val is None or env_backend_val.strip() == '':
    return 'cpu'
  else:
    if env_backend_val not in env_backend_options:
      print("Intel PyTorch Extension only supports CPU and GPU now.")
      sys.exit(1)
    else:
      return env_backend_val


def get_git_head_sha(base_dir):
  ipex_git_sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'],
                                        cwd=base_dir).decode('ascii').strip()
  if os.path.isdir(os.path.join(base_dir, '..', '.git')):
    torch_git_sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'],
                                            cwd=os.path.join(
                                                base_dir,
                                                '..')).decode('ascii').strip()
  else:
    torch_git_sha = ''
  return ipex_git_sha, torch_git_sha


def get_build_version(ipex_git_sha):
  version = os.getenv('TORCH_IPEX_VERSION', '0.1')
  if _check_env_flag('VERSIONED_IPEX_BUILD', default='0'):
    try:
      version += '+' + ipex_git_sha[:7]
    except Exception:
      pass
  return version


def create_version_files(base_dir, version, ipex_git_sha, torch_git_sha):
  print('Building torch_ipex version: {}'.format(version))
  # py_version_path = os.path.join(base_dir, 'intel_pytorch_extension_py', 'version.py')
  # with open(py_version_path, 'w') as f:
  #   f.write('# Autogenerated file, do not edit!\n')
  #   f.write("__version__ = '{}'\n".format(version))
  #   f.write("__ipex_gitrev__ = '{}'\n".format(ipex_git_sha))
  #   f.write("__torch_gitrev__ = '{}'\n".format(torch_git_sha))

  cpp_version_path = os.path.join(base_dir, 'torch_ipex', 'csrc', 'version.cpp')
  with open(cpp_version_path, 'w') as f:
    f.write('// Autogenerated file, do not edit!\n')
    f.write('#include "torch_ipex/csrc/version.h"\n\n')
    f.write('namespace torch_ipex {\n\n')
    f.write('const char IPEX_GITREV[] = {{"{}"}};\n'.format(ipex_git_sha))
    f.write('const char TORCH_GITREV[] = {{"{}"}};\n\n'.format(torch_git_sha))
    f.write('}  // namespace torch_ipex\n')


def generate_ipex_cpu_aten_code(base_dir):
  cur_dir = os.path.abspath(os.path.curdir)

  os.chdir(os.path.join(base_dir, 'scripts', 'cpu'))
  generate_code_cmd = ['./gen-cpu-sparse-dispatch.sh', os.path.join(base_dir, 'torch_ipex', 'csrc')]
  if subprocess.call(generate_code_cmd) != 0:
    print("Failed to run '{}'".format(generate_code_cmd), file=sys.stderr)
    os.chdir(cur_dir)
    sys.exit(1)

  generate_code_cmd = ['./gen-cpu-sparse-ops.sh', os.path.join(base_dir, 'torch_ipex', 'csrc', 'cpu')]
  if subprocess.call(generate_code_cmd) != 0:
    print("Failed to run '{}'".format(generate_code_cmd), file=sys.stderr)
    os.chdir(cur_dir)
    sys.exit(1)

  os.chdir(os.path.join(base_dir, 'scripts'))
  generate_code_cmd = ['./gen-cpu-ops.sh', os.path.join(base_dir, 'torch_ipex', 'csrc', 'cpu')]
  if subprocess.call(generate_code_cmd) != 0:
    print("Failed to run '{}'".format(generate_code_cmd), file=sys.stderr)
    os.chdir(cur_dir)
    sys.exit(1)

  os.chdir(cur_dir)


class DPCPPExt(Extension, object):
  def __init__(self, name, project_dir=os.path.dirname(__file__)):
    Extension.__init__(self, name, sources=[])
    self.project_dir = os.path.abspath(project_dir)
    self.build_dir = os.path.join(project_dir, 'build')

class install(setuptools.command.install.install):
    def run(self):
        setuptools.command.install.install.run(self)

class DPCPPClean(distutils.command.clean.clean, object):

  def run(self):
    import glob
    import re
    with open('.gitignore', 'r') as f:
      ignores = f.read()
      pat = re.compile(r'^#( BEGIN NOT-CLEAN-FILES )?')
      for wildcard in filter(None, ignores.split('\n')):
        match = pat.match(wildcard)
        if match:
          if match.group(1):
            # Marker is found and stop reading .gitignore.
            break
          # Ignore lines which begin with '#'.
        else:
          for filename in glob.glob(wildcard):
            try:
              os.remove(filename)
            except OSError:
              shutil.rmtree(filename, ignore_errors=True)

    # It's an old-style class in Python 2.7...
    distutils.command.clean.clean.run(self)


class DPCPPBuild(setuptools.command.build_ext.build_ext, object):
  def run(self):
    print("run")
    cmake = find_executable('cmake3') or find_executable('cmake')
    if cmake is None:
      raise RuntimeError(
          "CMake must be installed to build the following extensions: " +
              ", ".join(e.name for e in self.extensions))
    self.cmake = cmake

    if platform.system() == "Windows":
      raise RuntimeError("Does not support windows")

    for ext in self.extensions:
      self.build_extension(ext)

  def build_extension(self, ext):
    ext_dir = pathlib.Path(self.get_ext_fullpath(ext.name))
    if not os.path.exists(ext.build_dir):
      os.mkdir(ext.build_dir)

    build_type = 'Release'

    if _check_env_flag('DEBUG'):
      build_type = 'Debug'

    def convert_cmake_dirs(paths):
        def converttostr(input_seq, seperator):
            # Join all the strings in list
            final_str = seperator.join(input_seq)
            return final_str
        try:
            return converttostr(paths, ";")
        except:
            return paths

    def defines(args, **kwargs):
        for key, value in sorted(kwargs.items()):
            if value is not None:
                args.append('-D{}={}'.format(key, value))

    cmake_args = []
    build_options = {
        # The default value cannot be easily obtained in CMakeLists.txt. We set it here.
        # 'CMAKE_PREFIX_PATH': distutils.sysconfig.get_python_lib()
        'CMAKE_BUILD_TYPE': build_type,
        'PYTORCH_INCLUDE_DIR': convert_cmake_dirs(include_paths()),
        'PYTORCH_LIBRARY_DIR': convert_cmake_dirs(library_paths()),
        'PYTHON_EXECUTABLE': sys.executable,
        'CMAKE_INSTALL_PREFIX': '/'.join([str(ext_dir.parent.absolute()), ext.name]),
        'PYTHON_INCLUDE_DIR': distutils.sysconfig.get_python_inc(),
        'LIB_NAME': ext.name,
        'PYTHON_EXECUTABLE': sys.executable,
    }

    my_env = os.environ.copy()
    for var, val in my_env.items():
        if var.startswith(('BUILD_', 'USE_', 'CMAKE_')):
            build_options[var] = val

    cc, cxx = _get_complier()
    defines(cmake_args, CMAKE_C_COMPILER=cc)
    defines(cmake_args, CMAKE_CXX_COMPILER=cxx)
    defines(cmake_args, **build_options)
    cmake_args.append('-DUSE_SYCL=1')

    command = [self.cmake, ext.project_dir] + cmake_args
    print(' '.join(command))

    env = os.environ.copy()
    check_call(command, cwd=ext.build_dir, env=env)


    build_args = ['-j', str(multiprocessing.cpu_count()), 'install']

    # build_args += ['VERBOSE=1']
    check_call(['make'] + build_args, cwd=ext.build_dir, env=env)


ipex_git_sha, torch_git_sha = get_git_head_sha(base_dir)
version = get_build_version(ipex_git_sha)

# Generate version info (torch_xla.__version__)
create_version_files(base_dir, version, ipex_git_sha, torch_git_sha)

# Constant known variables used throughout this file

# PyTorch installed library
IS_WINDOWS = (platform.system() == 'Windows')
IS_DARWIN = (platform.system() == 'Darwin')
IS_LINUX = (platform.system() == 'Linux')

setup(
    name='torch_ipex',
    version=version,
    description='Intel PyTorch Extension',
    # url='https://github.com/pytorch/ipex',
    author='Intel/PyTorch Dev Team',
    # Exclude the build files.
    packages=['torch_ipex'],
    package_data={
        'torch_ipex': ['lib/*.so',
                       'include/*.h']},
    zip_safe=False,
    ext_modules=[DPCPPExt('torch_ipex')],
    cmdclass={
        'install' : install,
        'build_ext': DPCPPBuild,
        'clean': DPCPPClean,
    })
