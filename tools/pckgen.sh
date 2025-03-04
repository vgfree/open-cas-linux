#!/bin/bash
#
# Copyright(c) 2020-2022 Intel Corporation
# SPDX-License-Identifier: BSD-3-Clause
#

# To make additional packages creation possible
# through this script all you need to do is:
# - add 'case' command to set variable GENERATE_<NAME>="generate_<name>"
# - add function 'generate_<name>' to handle the task
# - add 'DEPENDENCIES_<NAME>' array if needed
# - update help!


CAS_NAME="open-cas-linux"
CAS_HOMEPAGE="https://open-cas.github.io"
CAS_GIT="https://github.com/Open-CAS/open-cas-linux.git"
CAS_LICENSE_NAME="BSD-3-Clause"
CAS_MODULES_DIR="extra/block/opencas"
SUPPORTED_FROM_VERSION="20.03"
THIS=$(basename "$0")
ARCH="$(uname -m)"
SCRIPT_BASE_DIR=$(dirname $(realpath "$0"))
RPM_SPEC_FILE="$SCRIPT_BASE_DIR/${THIS%.*}.d/rpm/CAS_NAME.spec"
DEB_CONTROL_FILES_DIR="$SCRIPT_BASE_DIR/${THIS%.*}.d/deb/debian"
PACKAGE_MAINTAINER="Rafal Stefanowski <rafal.stefanowski@intel.com>"
PACKAGE_DATE="$(date -R)"
TEMP_TEMPLATE="opencas-${THIS}"
DEPENDENCIES=(git mktemp rsync sed)
# Dependencies for particular packages creation:
DEPENDENCIES_TAR=(tar)
DEPENDENCIES_ZIP=(zip)
DEPENDENCIES_RPM=(rpmbuild tar)
DEPENDENCIES_SRPM=("${DEPENDENCIES_RPM[@]}")
DEPENDENCIES_DEB=(debuild dh fakeroot tar dkms)
DEPENDENCIES_DSC=("${DEPENDENCIES_DEB[@]}")
# List of relative submodule directories:
SUBMODULES=(
    "ocf"
)

# Unset all variables that may be checked for existence:
unset ${!GENERATE_*} ARCHIVE_PREPARED DEBUG FAILED_DEPS OUTPUT_DIR RPM_BUILT\
      SOURCES_DIR SUBMODULES_MISSING TAR_CREATED


usage() {
    echo "Usage:"
    echo "  ./$THIS <commands> [options] <SOURCES_PATH>"
    echo "  ./$THIS -c|-h"
    echo "where SOURCES_PATH is the root directory of OpenCAS sources"
}

print_help() {
    echo "Generate OpenCAS packages."
    echo "$(usage)"
    echo
    echo "This script generates various OpenCAS packages like"
    echo "release archives (tar, zip) and RPMs (source and binary)."
    echo
    echo "Mandatory arguments to long options are mandatory for short options too."
    echo
    echo "Commands:"
    echo "  tar                             generate tar archive"
    echo "  zip                             generate zip archive"
    echo "  rpm                             generate RPM packages"
    echo "  srpm                            generate SRPM (source RPM) package"
    echo "  deb                             generate DEB packages"
    echo "  dsc                             generate DSC (source DEB) package"
    echo
    echo "Options:"
    echo "  -a, --arch <ARCH>               target platform architecture for packages"
    echo "  -o, --output-dir <DIR>          put all created files in the given directory;"
    echo "                                  default: 'SOURCES_PATH/packages/'"
    echo "  -d, --debug                     include debug information and create debug packages"
    echo "  -c, --clean                     clean all temporary files and folders that"
    echo "                                  may have been left around if $THIS ended"
    echo "                                  unexpectedly in the previous run"
    echo "  -h, --help                      print this help message"
    echo
}

invalid_usage() {
    echo -e "$THIS: $*\nTry './$THIS --help' for more information." >&2
    exit 2
}

info() {
    echo -e "\e[33m$*\e[0m"
}

error() {
    echo -e "\e[31mERROR\e[0m: $THIS: $*" >&2
    exit 1
}

clean() {
    rm -rf "$TEMP_DIR"
    if [ -d "$TEMP_DIR" ]; then
        # Print only warning to not confuse the user by an error here
        # if packages were created successfully and everything else
        # went fine except cleaning temp directory at the end.
        info "WARNING: Cleanup failed" >&2
    fi
}

clean_all() {
    info "Removing all temp files and dirs that may have been left around..."
    rm -rf "/tmp/${TEMP_TEMPLATE}."*
    if ls "/tmp/${TEMP_TEMPLATE}."* &>/dev/null; then
        # This function on the other hand is called only by a '-c' option
        # so we may throw an error here and exit.
        error "cleanup failed"
    fi
}

check_os() {
    source "/etc/os-release"

    echo "$ID_LIKE"
}

check_options() {
    if [ ! "$SOURCES_DIR" ]; then
        invalid_usage "no mandatory SOURCES_PATH provided"
    elif [[ $(head -n 1 "$SOURCES_DIR/README.md" 2>/dev/null) != *Open*CAS*Linux* ]]; then
        invalid_usage "'$SOURCES_DIR' does not point to the root directory of CAS sources"
    elif [ ! "${!GENERATE_*}" ]; then
        invalid_usage "nothing to do - no command provided"
    fi
}

check_version() {
    if ! (cd $(dirname "$CAS_VERSION_GEN") && ./$(basename "$CAS_VERSION_GEN")); then
        error "failed to obtain CAS version"
    fi
    . "$VERSION_FILE"

    VERSION_SHORT="${CAS_VERSION_MAIN}.$(printf %02d ${CAS_VERSION_MAJOR})"
    if [ $CAS_VERSION_MINOR -ne 0 ]; then
        VERSION_SHORT+=".${CAS_VERSION_MINOR}"
    fi
    if [[ "$VERSION_SHORT" < "$SUPPORTED_FROM_VERSION" ]]; then
        echo "Sorry... this version of $CAS_NAME ($VERSION_SHORT) is not supported"\
             "by $THIS. Use $CAS_NAME >= $SUPPORTED_FROM_VERSION"
        exit 1
    fi

    for SUBMOD in ${SUBMODULES[@]}; do
        if ! ls -A "$SOURCES_DIR/$SUBMOD/"* &>/dev/null; then
            local SUBMODULES_MISSING+="'$SUBMOD'\n"
        fi
    done
    if [ "$SUBMODULES_MISSING" ]; then
        error "There are missing submodules:\n${SUBMODULES_MISSING}\nUpdate submodules and try again!"
    fi
}

check_dependencies() {
    echo "--- Checking for dependencies"
    for package in ${!GENERATE_*}; do
        local DEP_NAME="DEPENDENCIES_${package##*_}[@]"
        DEPENDENCIES+=(${!DEP_NAME})
    done
    for DEP in ${DEPENDENCIES[@]}; do
        if ! which $DEP &>/dev/null; then
            local FAILED_DEPS+="$DEP "
        fi
    done

    if [ "$FAILED_DEPS" ]; then
        error "Dependencies not installed. You need to provide these programs first: $FAILED_DEPS"
    fi
}

create_dir() {
    mkdir -p "$*"
    if [ ! -d "$*" ] || [ ! -w "$*" ]; then
        error "no access to '$*'"
    fi
}

create_temp() {
    TEMP_DIR=$(mktemp -d -t ${TEMP_TEMPLATE}.XXXXXXXXXX)
    if [ $? -ne 0 ]; then
        error "couldn't create temporary directory"
    fi
}

rename_templates() {
    # Due to inconsistent ordering in 'find' output, it is necessary to
    # rerun files renaming in order to include files and subdirectories
    # when both need to be renamed. To prevent infinite loops, rerun is not
    # based on 'mv' command exit status, but on specified subdirectory depth.
    # This mechanism seems to be much simpler then piping the output through
    # various external utilities.
    SUBDIR_DEPTH=3
    while [ $((SUBDIR_DEPTH--)) -gt 0 ]; do
        for filename in $(find "$@"); do
            if [[ "$filename" =~ CAS_NAME ]]; then
                mv "$filename" "${filename//CAS_NAME/${CAS_NAME}}" 2>/dev/null
            fi
        done
    done
}

archive_prepare() {
    if [ "$ARCHIVE_PREPARED" ]; then
        return 0
    fi

    echo "--- Copying sources to the working directory and cleaning"
    local TEMP_SOURCES_DIR="$TEMP_DIR/$CAS_FILENAME"
    rm -rf "$TEMP_SOURCES_DIR"
    mkdir -p "$TEMP_SOURCES_DIR"
    rsync -a --exclude={/packages,.git*,.pep8speaks.yml} "$SOURCES_DIR/" "$TEMP_SOURCES_DIR"
    make -C "$TEMP_SOURCES_DIR" clean distclean >/dev/null

    ARCHIVE_PREPARED="archive_prepared"
}

generate_tar() {
    if [ ! "$TAR_CREATED" ]; then
        archive_prepare
        echo "--- Creating tar archive from current sources"
        tar -C "$TEMP_DIR" -zcf "$TEMP_DIR/$SOURCES_TAR_NAME" "$CAS_FILENAME"
        if [ $? -ne 0 ] || [ ! -f "$TEMP_DIR/$SOURCES_TAR_NAME" ]; then
            rm -rf "$TEMP_DIR/$SOURCES_TAR_NAME"
            error "couldn't create tar archive"
        fi

        TAR_CREATED="tar_created"
    fi

    if [ "$1" != "temp" ]; then
        cp "$TEMP_DIR/$SOURCES_TAR_NAME" "$OUTPUT_DIR"
    fi
}

generate_zip() {
    archive_prepare
    echo "--- Creating zip archive from current sources"
    (cd "$TEMP_DIR" && zip -qr - "$CAS_FILENAME") > "$OUTPUT_DIR/$SOURCES_ZIP_NAME"
    if [ $? -ne 0 ] || [ ! -f "$OUTPUT_DIR/$SOURCES_ZIP_NAME" ]; then
        rm -rf "$OUTPUT_DIR/$SOURCES_ZIP_NAME"
        error "couldn't create zip archive"
    fi
}

rpm_create_tree() {
    echo "--- Creating directory tree for building RPMs"
    mkdir -p "$RPM_BUILD_DIR/"{BUILD,RPMS,SOURCES,SPECS,SRPMS}
    if [ $? -ne 0 ] || [ ! -w "$RPM_BUILD_DIR" ]; then
        error "couldn't create directory tree for building RPMs"
    fi
}

rpm_obtain_sources() {
    echo "--- Obtaining CAS sources for RPMs"
    generate_tar temp
    cp -v "$TEMP_DIR/$SOURCES_TAR_NAME" "$RPM_SOURCES_DIR"

    if [ ! -f "$RPM_SOURCES_DIR/$SOURCES_TAR_NAME" ]; then
        error "couldn't obtain $SOURCES_TAR_NAME sources tarball!"
    fi
}

deb_obtain_sources() {
    echo "--- Obtaining CAS sources for DEBs"
    generate_tar temp
    mkdir -p "$DEB_BUILD_DIR"
    cp -v "$TEMP_DIR/$SOURCES_TAR_NAME" "$DEB_BUILD_DIR/$DEB_TAR_NAME"

    if [ ! -f "$DEB_BUILD_DIR/$DEB_TAR_NAME" ]; then
        error "couldn't obtain $DEB_TAR_NAME sources tarball!"
    fi

    tar -C "$DEB_BUILD_DIR" -zxf "$DEB_BUILD_DIR/$DEB_TAR_NAME"
    if [ $? -ne 0 ] || [ ! -d "$DEB_SOURCES_DIR" ]; then
        error "couldn't unpack tar archive '$DEB_BUILD_DIR/$DEB_TAR_NAME'"\
              "or it contains wrong sources dir name"\
              "(should be '$(basename $DEB_SOURCES_DIR)')"
    fi
}

rpm_spec_prepare() {
    echo "--- Preparing SPEC file"
    if [ ! -f "$RPM_SPEC_FILE" ]; then
        error "SPEC file '$RPM_SPEC_FILE' not found"
    fi

    cp "$RPM_SPEC_FILE" "$RPM_SPECS_DIR"
    if ! ls -A "$RPM_SPECS_DIR/"* &>/dev/null; then
        error "couldn't copy SPEC file to working directory '$RPM_SPECS_DIR'"
    fi

    rename_templates "$RPM_SPECS_DIR"

    sed -i "s/<CAS_NAME>/$CAS_NAME/g" "$RPM_SPECS_DIR/$CAS_NAME.spec"
    sed -i "s/<CAS_VERSION>/$CAS_VERSION/g" "$RPM_SPECS_DIR/$CAS_NAME.spec"
    sed -i "s/<LAST_COMMIT_HASH_ABBR>/$LAST_COMMIT_HASH_ABBR/g" "$RPM_SPECS_DIR/$CAS_NAME.spec"
    sed -i "s/<CAS_LICENSE_NAME>/$CAS_LICENSE_NAME/g" "$RPM_SPECS_DIR/$CAS_NAME.spec"
    sed -i "s|<CAS_MODULES_DIR>|$CAS_MODULES_DIR|g" "$RPM_SPECS_DIR/$CAS_NAME.spec"
    sed -i "s/<CAS_HOMEPAGE>/${CAS_HOMEPAGE//\//\\/}/g" "$RPM_SPECS_DIR/$CAS_NAME.spec"
    sed -i "s/<PACKAGE_MAINTAINER>/$PACKAGE_MAINTAINER/g" "$RPM_SPECS_DIR/$CAS_NAME.spec"

    if [ "$DEBUG" ]; then
        echo "---   Debug info will be included and debug packages created as well"

        sed -i "s/<MAKE_BUILD>/%make_build DEBUG_PACKAGE=1/g" "$RPM_SPECS_DIR/$CAS_NAME.spec"
        sed -i "/<DEBUG_PACKAGE>/d" "$RPM_SPECS_DIR/$CAS_NAME.spec"
        if [[ $(check_os) =~ suse|sles ]]; then
            sed -i "/%prep/i %debug_package\n\n" "$RPM_SPECS_DIR/$CAS_NAME.spec"
        fi
    else
        sed -i "s/<MAKE_BUILD>/%make_build/g" "$RPM_SPECS_DIR/$CAS_NAME.spec"
        sed -i "s/<DEBUG_PACKAGE>/%define debug_package %{nil}/g" "$RPM_SPECS_DIR/$CAS_NAME.spec"
    fi

    if [ ! -f "$RPM_SPECS_DIR/$CAS_NAME.spec" ]; then
        error "couldn't create a SPEC file"
    fi
}

deb_control_files_prepare() {
    echo "--- Preparing DEB control files"
    if ! ls -A "$DEB_CONTROL_FILES_DIR/"* &>/dev/null; then
        error "DEB control files directory '$DEB_CONTROL_FILES_DIR' not found or empty"
    fi

    cp -r "$DEB_CONTROL_FILES_DIR" "$DEB_SOURCES_DIR"
    if ! ls -A "$DEB_SOURCES_DIR/debian/"* &>/dev/null; then
        error "couldn't copy DEB control files to working directory '$DEB_SOURCES_DIR'"
    fi

    rename_templates "$DEB_SOURCES_DIR/debian/"

    # Parsing the CAS license file to fit Debian copyright file format
    sed '${/^$/d}' "$CAS_LICENSE" > "$TEMP_DIR/LICENSE.deb.tmp"
    sed -i 's/^$/./' "$TEMP_DIR/LICENSE.deb.tmp"
    rm -f "$TEMP_DIR/LICENSE.deb"
    while read -r line; do
        echo " $line" >> "$TEMP_DIR/LICENSE.deb"
    done < "$TEMP_DIR/LICENSE.deb.tmp"
    rm -f "$TEMP_DIR/LICENSE.deb.tmp"
    cat "$TEMP_DIR/LICENSE.deb" >> "$DEB_SOURCES_DIR/debian/copyright"

    # Replacing tags
    for file in $(find "$DEB_SOURCES_DIR/debian/" -type f); do
        sed -i "s/<CAS_NAME>/$CAS_NAME/g" "$file"
        sed -i "s/<CAS_VERSION>/$CAS_VERSION/g" "$file"
        sed -i "s/<CAS_LICENSE_NAME>/$CAS_LICENSE_NAME/g" "$file"
        sed -i "s|<CAS_MODULES_DIR>|$CAS_MODULES_DIR|g" "$file"
        sed -i "s/<CAS_HOMEPAGE>/${CAS_HOMEPAGE//\//\\/}/g" "$file"
        sed -i "s/<CAS_GIT>/${CAS_GIT//\//\\/}/g" "$file"
        sed -i "s/<PACKAGE_MAINTAINER>/$PACKAGE_MAINTAINER/g" "$file"
        sed -i "s/<PACKAGE_DATE>/$PACKAGE_DATE/g" "$file"
        sed -i "s/<YEAR>/$(date +%Y)/g" "$file"
    done

    if [ "$DEBUG" ]; then
        echo "---   Debug info will be included and debug packages created as well"

        sed -i "s/<MAKE_BUILD>/make -C casadm DEBUG_PACKAGE=1/g" "$DEB_SOURCES_DIR/debian/rules"
        sed -i "s/<DEBUG_PACKAGE>/dh_strip --ddebs/g" "$DEB_SOURCES_DIR/debian/rules"
    else
        sed -i "s/<MAKE_BUILD>/make -C casadm/g" "$DEB_SOURCES_DIR/debian/rules"
        sed -i "s/<DEBUG_PACKAGE>/dh_strip --no-ddebs/g" "$DEB_SOURCES_DIR/debian/rules"
    fi
}

generate_rpm() {
    if [ "$RPM_BUILT" ]; then
        return 0
    fi

    rpm_create_tree
    rpm_obtain_sources
    rpm_spec_prepare

    if [[ $(check_os) =~ suse|sles ]] && [ -d /usr/src/packages ]; then
        info "INFO: It appears that you are using SUSE Linux."\
             "In case of encountering error during building of RPM package,"\
             "about missing files or directories in /usr/src/packages/,"\
             "remove or rename '/usr/src/packages' folder to prevent"\
             "rpmbuild to look for stuff in that location."
    fi

    if [ ! "$GENERATE_SRPM" ] && [ "$GENERATE_RPM" ]; then
        echo "--- Building binary RPM packages"
        (HOME="$TEMP_DIR"; rpmbuild -bb --target "$ARCH" "$RPM_SPECS_DIR/$CAS_NAME.spec")
        if [ $? -ne 0 ]; then
            error "couldn't create RPM packages"
        fi
        mv -ft "$OUTPUT_DIR" "$RPM_RPMS_DIR/$ARCH"/*
    fi
    if [ "$GENERATE_SRPM" ] && [ ! "$GENERATE_RPM" ]; then
        echo "--- Building source SRPM package"
        (HOME="$TEMP_DIR"; rpmbuild -bs "$RPM_SPECS_DIR/$CAS_NAME.spec")
        if [ $? -ne 0 ]; then
            error "couldn't create SRPM package"
        fi
        mv -ft "$OUTPUT_DIR" "$RPM_SRPMS_DIR"/*
    fi
    if [ "$GENERATE_SRPM" ] && [ "$GENERATE_RPM" ]; then
        echo "--- Building source and binary RPM packages"
        (HOME="$TEMP_DIR"; rpmbuild -ba --target "$ARCH" "$RPM_SPECS_DIR/$CAS_NAME.spec")
        if [ $? -ne 0 ]; then
            error "couldn't create RPM packages"
        fi
        mv -ft "$OUTPUT_DIR" "$RPM_SRPMS_DIR"/*
        mv -ft "$OUTPUT_DIR" "$RPM_RPMS_DIR/$ARCH"/*
    fi

    RPM_BUILT="rpm_built"
}

generate_srpm() {
    generate_rpm
}

generate_deb() {
    if [ "$DEB_BUILT" ]; then
        return 0
    fi

    deb_obtain_sources
    deb_control_files_prepare

    if [ ! "$GENERATE_DSC" ] && [ "$GENERATE_DEB" ]; then
        echo "--- Building binary DEB packages"
        (cd "$DEB_SOURCES_DIR" && debuild -us -uc -b --host-type "$ARCH-linux-gnu")
        if [ $? -ne 0 ]; then
            error "couldn't create DEB packages"
        fi
        mv -ft "$OUTPUT_DIR" "$DEB_BUILD_DIR/$CAS_NAME"*deb
    fi
    if [ "$GENERATE_DSC" ] && [ ! "$GENERATE_DEB" ]; then
        echo "--- Building DSC (source DEB) package"
        (cd "$DEB_SOURCES_DIR" && debuild -us -uc -S -d)
        if [ $? -ne 0 ]; then
            error "couldn't create DSC package"
        fi
        mv -ft "$OUTPUT_DIR" "$DEB_BUILD_DIR/$CAS_NAME"*.dsc
        mv -ft "$OUTPUT_DIR" "$DEB_BUILD_DIR/$CAS_NAME"*.orig.tar.gz
        mv -ft "$OUTPUT_DIR" "$DEB_BUILD_DIR/$CAS_NAME"*.debian.tar.xz
    fi
    if [ "$GENERATE_DSC" ] && [ "$GENERATE_DEB" ]; then
        echo "--- Building DEB and DSC (source DEB) packages"
        (cd "$DEB_SOURCES_DIR" && debuild -us -uc -F --host-type "$ARCH-linux-gnu")
        if [ $? -ne 0 ]; then
            error "couldn't create DEB and DSC packages"
        fi
        mv -ft "$OUTPUT_DIR" "$DEB_BUILD_DIR/$CAS_NAME"*deb
        mv -ft "$OUTPUT_DIR" "$DEB_BUILD_DIR/$CAS_NAME"*.dsc
        mv -ft "$OUTPUT_DIR" "$DEB_BUILD_DIR/$CAS_NAME"*.orig.tar.gz
        mv -ft "$OUTPUT_DIR" "$DEB_BUILD_DIR/$CAS_NAME"*.debian.tar.xz
    fi

    DEB_BUILT="deb_built"
}

generate_dsc() {
    generate_deb
}


if (( ! $# )); then
    invalid_usage "no arguments given\n$(usage)\n"
fi

while (( $# )); do
    case "$1" in
        tar)
            GENERATE_TAR="generate_tar"
            ;;
        zip)
            GENERATE_ZIP="generate_zip"
            ;;
        rpm)
            GENERATE_RPM="generate_rpm"
            ;;
        srpm)
            GENERATE_SRPM="generate_srpm"
            ;;
        deb)
            GENERATE_DEB="generate_deb"
            ;;
        dsc)
            GENERATE_DSC="generate_dsc"
            ;;
        --arch|-a)
            ARCH="$2"
            shift
            ;;
        --output-dir|-o)
            OUTPUT_DIR="$2"
            if ! dirname $OUTPUT_DIR &>/dev/null; then
                invalid_usage "no output directory given after the '--output-dir' option"
            fi
            shift
            ;;
        --debug|-d)
            DEBUG="debug"
            ;;
        --clean|-c)
            clean_all
            exit 0
            ;;
        --help|-h)
            print_help
            exit 0
            ;;
        *)
            if [ -d "$1" ]; then
                SOURCES_DIR=$(realpath "$1")
            else
                invalid_usage "option '$1' not recognized"
            fi
            ;;
    esac
    shift
done

check_options

# Following line removes all temporary (build process) files and dirs
# even if this script ends with an error. For debuging purposes, when
# build-time files are needed for inspection, simply comment it out.
trap clean EXIT

create_temp


### Variables that relates on arguments passed to this script:

CAS_LICENSE="$SOURCES_DIR/LICENSE"
# By default all created packages will be put in:
: ${OUTPUT_DIR:="$SOURCES_DIR/packages"}
# RPM building directories:
RPM_BUILD_DIR="$TEMP_DIR/rpmbuild"
RPM_SOURCES_DIR="$RPM_BUILD_DIR/SOURCES"
RPM_SPECS_DIR="$RPM_BUILD_DIR/SPECS"
RPM_RPMS_DIR="$RPM_BUILD_DIR/RPMS"
RPM_SRPMS_DIR="$RPM_BUILD_DIR/SRPMS"
DEB_BUILD_DIR="$TEMP_DIR/debuild"
# Version file location:
VERSION_FILE="$SOURCES_DIR/.metadata/cas_version"
# CAS version generator location:
CAS_VERSION_GEN="$SOURCES_DIR/tools/cas_version_gen.sh"

check_version

# CAS naming convention:
CAS_FILENAME="$CAS_NAME-$CAS_VERSION"
# CAS sources archives filenames:
SOURCES_TAR_NAME="$CAS_FILENAME.tar.gz"
SOURCES_ZIP_NAME="$CAS_FILENAME.zip"
DEB_TAR_NAME="${CAS_NAME}_${CAS_VERSION}.orig.tar.gz"
# DEB sources dir needs to be obtained after version checking
# because its name contains version number
DEB_SOURCES_DIR="$DEB_BUILD_DIR/$CAS_NAME-$CAS_VERSION"



#
# Run the package generator script
#

info "\n=== Running OpenCAS '$CAS_VERSION' package generator ===\n"

echo -n "Packages that will be built: "
for package in ${!GENERATE_*}; do
    echo -en "\e[33m${package##*_}\e[0m "
done
echo -e "\n"

check_dependencies
create_dir "$OUTPUT_DIR"
for package in ${!GENERATE_*}; do
    ${package,,}
done

echo -e "\n\e[32m=== ALL DONE ===\e[0m\n\nYou can find your fresh packages in '$OUTPUT_DIR'\n"
