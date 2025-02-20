#!/bin/bash
# Copyright 2022 Google LLC.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.



# Create the Yggdrasil Decision Forests pip package.
# This command uses the compiled artifacts generated by tools/test_pydf.sh.
# It should be run from the PYDF workspace root.
#
# Usage example:
#   # Generate the pip package with python3.9
#   ./tools/build_pydf.sh python3.9
#
#   # Generate the pip package for all the versions of python using pyenv.
#   # Make sure the package are compatible with manylinux2014.
#   ./tools/build_pip_package.sh ALL_VERSIONS
#
#   TODO: Add compilation on MacOS and Windows
#
# Requirements:
#
#   pyenv (if using ALL_VERSIONS_ALREADY_ASSEMBLED or ALL_VERSIONS)
#     See https://github.com/pyenv/pyenv-installer
#     Will be installed by this script if INSTALL_PYENV is set to INSTALL_PYENV.
#
#   Auditwheel
#     Auditwheel is required for Linux builds.
#     Auditwheel needs to be version 5.2.0. The script will attempt to
#     update Auditwheel to this version.
#

set -xve

PLATFORM="$(uname -s | tr 'A-Z' 'a-z')"
function is_macos() {
  [[ "${PLATFORM}" == "darwin" ]]
}

# Temporary directory used to assemble the package.
SRCPK="$(pwd)/tmp_package"

function check_auditwheel() {
  PYTHON="$1"
  shift
  local auditwheel_version="$(${PYTHON} -m pip show auditwheel | grep "Version:")"
  if [ "$auditwheel_version" != "Version: 5.2.0" ]; then
   echo "Auditwheel needs to be Version 5.2.0, currently ${auditwheel_version}"
   exit 1
  fi
}

# Pypi package version compatible with a given version of python.
# Example: Python3.8.2 => Package version: "38"
function python_to_package_version() {
  PYTHON="$1"
  shift
  ${PYTHON} -c 'import sys; print(f"{sys.version_info.major}{sys.version_info.minor}")'
}

# Installs dependency requirement for build the Pip package.
function install_dependencies() {
  PYTHON="$1"
  shift
  ${PYTHON} -m ensurepip -U || true
  ${PYTHON} -m pip install pip -U
  ${PYTHON} -m pip install setuptools -U
  ${PYTHON} -m pip install build -U
  ${PYTHON} -m pip install virtualenv -U
  ${PYTHON} -m pip install auditwheel==5.2.0
}

function check_is_build() {
  # Check the correct location of the current directory.
  local cur_dir=${PWD##*/}
  if [ ! -d "bazel-bin" ] || [ $cur_dir != "python" ]; then
    echo "This script should be run from the root directory of the Python port of Yggdrasil Decision Forests (i.e. probably under port/python) of a compiled Bazel export (i.e. containing a bazel-bin directory)"
    exit 1
  fi
}

# Collects the library files into ${SRCPK}
function assemble_files() {
  check_is_build

  rm -fr ${SRCPK}
  mkdir -p ${SRCPK}
  cp -R ydf config/setup.py config/MANIFEST.in README.md CHANGELOG.md ${SRCPK}

  # When cross-compiling, adapt the platform string.
  if [ ${ARG} == "ALL_VERSIONS_MAC_INTEL_CROSSCOMPILE" ]; then
    sed -i'.bak' -e "s/# plat = \"macosx_10_15_x86_64\"/plat = \"macosx_10_15_x86_64\"/" ${SRCPK}/setup.py
  fi

  # YDF's wrappers and .so.
  SRCBIN="bazel-bin/ydf"
  cp ${SRCBIN}/cc/ydf.so ${SRCPK}/ydf/cc/

  cp ${SRCBIN}/learner/specialized_learners.py ${SRCPK}/ydf/learner/

  # YDF's proto wrappers.
  YDFSRCBIN="bazel-bin/external/ydf_cc/yggdrasil_decision_forests"
  mkdir -p ${SRCPK}/yggdrasil_decision_forests
  pushd ${YDFSRCBIN}
  find . -name \*.py -exec rsync -R -arv {} ${SRCPK}/yggdrasil_decision_forests \;
  popd

  # Copy the license file from YDF
  cp bazel-python/external/ydf_cc/LICENSE ${SRCPK}

  # Add __init__.py to all exported Yggdrasil sub-directories.
  find ${SRCPK}/yggdrasil_decision_forests -type d -exec touch {}/__init__.py \;

  # Absl status wrapper
  ABSLSRCBIN="bazel-bin/external/com_google_pybind11_abseil/pybind11_abseil"
  mkdir -p ${SRCPK}/pybind11_abseil
  touch ${SRCPK}/pybind11_abseil/__init__.py
  cp ${ABSLSRCBIN}/status.so ${SRCPK}/pybind11_abseil
}

# Build a pip package.
function build_package() {
  PYTHON="$1"
  shift

  pushd ${SRCPK}
  $PYTHON -m build
  popd

  cp -R ${SRCPK}/dist .
}

# Tests a pip package.
function test_package() {
  if [ ${ARG} == "ALL_VERSIONS_MAC_INTEL_CROSSCOMPILE" ]; then
    echo "Cross-compiled packages cannot be tested on the machine they're built with."
    return
  fi
  PYTHON="$1"
  shift
  PACKAGE="$1"
  shift

  PIP="${PYTHON} -m pip"

  if is_macos; then
    PACKAGEPATH="dist/ydf-*-cp${PACKAGE}-cp${PACKAGE}*-*.whl"
  else
    PACKAGEPATH="dist/ydf-*-cp${PACKAGE}-cp${PACKAGE}*.manylinux2014_x86_64.whl"
  fi
  ${PIP} install ${PACKAGEPATH}


  ${PIP} list
  ${PIP} show ydf -f

  # Run a small example (in different folder to avoid clashes)
  # TODO: Implement a small test.
  pushd ..
  ${PYTHON} -c "import ydf"
  popd

  # rm -rf previous_package
  # mkdir previous_package
  # ${PYTHON} -m pip download --no-deps -d previous_package ydf
  # local old_file_size=`du -k "previous_package" | cut -f1`
  # local new_file_size=`du -k $PACKAGEPATH | cut -f1`
  # local scaled_old_file_size=$(($old_file_size * 12))
  # local scaled_new_file_size=$(($new_file_size * 10))
  # if [ "$scaled_new_file_size" -gt "$scaled_old_file_size" ]; then
  #   echo "New package is 20% larger than the previous one."
  #   echo "This probably indicates a problem, aborting."
  #   exit 1
  # fi
  # scaled_old_file_size=$(($old_file_size * 8))
  # if [ "$scaled_new_file_size" -lt "$scaled_old_file_size" ]; then
  #   echo "New package is 20% smaller than the previous one."
  #   echo "This probably indicates a problem, aborting."
  #   exit 1
  # fi
}

# Builds and tests a pip package in a given version of python
function e2e_native() {
  PYTHON="$1"
  shift
  PACKAGE=$(python_to_package_version ${PYTHON})

  install_dependencies ${PYTHON}
  build_package ${PYTHON}

  # Fix package.
  if is_macos; then
    PACKAGEPATH="dist/ydf-*-cp${PACKAGE}-cp${PACKAGE}*-*.whl"
  else
    check_auditwheel ${PYTHON}
    PACKAGEPATH="dist/ydf-*-cp${PACKAGE}-cp${PACKAGE}*-linux_x86_64.whl"
    ${PYTHON} -m auditwheel repair --plat manylinux2014_x86_64 -w dist ${PACKAGEPATH}
  fi

  test_package ${PYTHON} ${PACKAGE}
}

# Builds and tests a pip package in Pyenv.
function e2e_pyenv() {
  VERSION="$1"
  shift

  # Don't force updating pyenv, we use a fixed version.
  # pyenv update

  ENVNAME=env_${VERSION}
  pyenv install ${VERSION} -s

  # Enable pyenv virtual environment.
  set +e
  pyenv virtualenv ${VERSION} ${ENVNAME}
  set -e
  pyenv activate ${ENVNAME}

  e2e_native python3

  # Disable virtual environment.
  pyenv deactivate
}

ARG="$1"
INSTALL_PYENV="$2"
shift | true

if [ ${INSTALL_PYENV} == "INSTALL_PYENV" ]; then 
  if ! [ -x "$(command -v pyenv)" ]; then
    echo "Pyenv not found."
    echo "Installing build deps, pyenv 2.3.7 and pyenv virtualenv 1.2.1"
    # Install python dependencies.
    if ! is_macos; then
      sudo apt-get update
      sudo apt-get install -qq make build-essential libssl-dev zlib1g-dev \
                libbz2-dev libreadline-dev libsqlite3-dev wget curl llvm \
                libncursesw5-dev xz-utils tk-dev libxml2-dev libxmlsec1-dev \
                libffi-dev liblzma-dev patchelf
    fi
    git clone https://github.com/pyenv/pyenv.git
    (
      cd pyenv && git checkout 74f923b5fca82054b3c579f9eb936338c7f5a394
    )
    PYENV_ROOT="$(pwd)/pyenv"
    export PATH="$PYENV_ROOT/bin:$PATH"
    eval "$(pyenv init --path)"
    eval "$(pyenv init -)"
    git clone https://github.com/pyenv/pyenv-virtualenv.git $(pyenv root)/plugins/pyenv-virtualenv
    (
      cd $(pyenv root)/plugins/pyenv-virtualenv && git checkout 13bc1877ef06ed038c65dcab4e901da6ea6c67ae
    )
    eval "$(pyenv init --path)"
    eval "$(pyenv init -)"
    eval "$(pyenv virtualenv-init -)"
  fi
fi

if [ -z "${ARG}" ]; then
  echo "The first argument should be one of:"
  echo "  ALL_VERSIONS: Build all pip packages using pyenv."
  echo "  ALL_VERSIONS_ALREADY_ASSEMBLED: Build all pip packages from already assembled files using pyenv."
  echo "  Python binary (e.g. python3.9): Build a pip package for a specific python version without pyenv."
  exit 1
elif [ ${ARG} == "ALL_VERSIONS" ]; then
  # Compile with all the version of python using pyenv.
  assemble_files
  eval "$(pyenv init -)"
  e2e_pyenv 3.9.12
  e2e_pyenv 3.10.4
  e2e_pyenv 3.11.0
elif [ ${ARG} == "ALL_VERSIONS_ALREADY_ASSEMBLED" ]; then
  eval "$(pyenv init -)"
  e2e_pyenv 3.9.12
  e2e_pyenv 3.10.4
  e2e_pyenv 3.11.0
elif [ ${ARG} == "ALL_VERSIONS_MAC_INTEL_CROSSCOMPILE" ]; then
  eval "$(pyenv init -)"
  assemble_files
  e2e_pyenv 3.9.12
  e2e_pyenv 3.10.4
  e2e_pyenv 3.11.0
else
  # Compile with a specific version of python provided in the call arguments.
  assemble_files
  PYTHON=${ARG}
  e2e_native ${PYTHON}
fi
