#  -*- mode: cmake -*-

#
# Build TPL: Nanoflann 
# 

# --- Define all the directories and common external project flags
define_external_project_args(Nanoflann 
                             TARGET nanoflann)

# add version version to the autogenerated tpl_versions.h file
amanzi_tpl_version_write(FILENAME ${TPL_VERSIONS_INCLUDE_FILE}
                         PREFIX Nanoflann
                         VERSION ${Nanoflann_VERSION_MAJOR} ${Nanoflann_VERSION_MINOR} ${Nanoflann_VERSION_PATCH})

# --- Define the install directory
set(nanoflann_install_dir ${TPL_INSTALL_PREFIX})

# --- Add external project and tie to the Nanoflann build target
ExternalProject_Add(${Nanoflann_BUILD_TARGET}
                    TMP_DIR   ${Nanoflann_tmp_dir}         # Temporary files directory
                    STAMP_DIR ${Nanoflann_stamp_dir}       # Timestamp and log directory
                    # -- Download and URL definitions
                    DOWNLOAD_DIR  ${TPL_DOWNLOAD_DIR}      
                    URL           ${Nanoflann_URL}          # URL may be a web site OR a local file
                    URL_MD5       ${Nanoflann_MD5_SUM}      # md5sum of the archive file
                    DOWNLOAD_NAME ${Nanoflann_SAVEAS_FILE}  # file name to store (if not end of URL)
                    # -- Configure
                    SOURCE_DIR    ${Nanoflann_source_dir}  # Source directory
                    CMAKE_ARGS    ${AMANZI_CMAKE_CACHE_ARGS}   # Ensure uniform build
                                 -DCMAKE_C_FLAGS:STRING=${Amanzi_COMMON_CFLAGS}  # Ensure uniform build
                                 -DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}
                                 -DCMAKE_CXX_FLAGS:STRING=${Amanzi_COMMON_CXXFLAGS}
                                 -DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}
                                 -DCMAKE_INSTALL_PREFIX:PATH=${nanoflann_install_dir}
                    # -- Build
                    BINARY_DIR    ${Nanoflann_build_dir}   # Build directory 
                    # -- Install
                    INSTALL_DIR   ${nanoflann_install_dir} 
                    # -- Output control
                    ${Nanoflann_logging_args})

