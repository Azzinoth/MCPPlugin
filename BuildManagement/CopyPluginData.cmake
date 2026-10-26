# Copies files that plugin needs at runtime, but that are not build outputs, to Build folder.
# Expects SOURCE_DIRECTORY, BINARY_DIRECTORY and PLUGIN_NAME to be passed with -D.
cmake_minimum_required(VERSION 3.15)

# Destination becomes the same as source: new and changed files are copied, files that are no longer in source are removed.
# Copy keeps timestamps, so unchanged files are skipped next time.
function(MirrorDirectory Source Destination)
    if(NOT EXISTS "${Source}")
        file(REMOVE_RECURSE "${Destination}")
        return()
    endif()

    file(GLOB_RECURSE SourceFiles RELATIVE "${Source}" "${Source}/*")
    list(FILTER SourceFiles EXCLUDE REGEX "(^|/)__pycache__/")

    file(GLOB_RECURSE RemovedFiles RELATIVE "${Destination}" "${Destination}/*")
    if(RemovedFiles AND SourceFiles)
        list(REMOVE_ITEM RemovedFiles ${SourceFiles})
    endif()

    foreach(File ${RemovedFiles})
        file(REMOVE "${Destination}/${File}")
    endforeach()

    file(COPY "${Source}/" DESTINATION "${Destination}" PATTERN "__pycache__" EXCLUDE)
endfunction()

file(COPY "${SOURCE_DIRECTORY}/${PLUGIN_NAME}.json" DESTINATION "${SOURCE_DIRECTORY}/Build")
MirrorDirectory("${SOURCE_DIRECTORY}/Python" "${SOURCE_DIRECTORY}/Build/Data/Python")

# Checking every package file takes seconds, but packages change only when they are installed again from requirements file,
# so they are copied only when that file changed, deleting Build/Data/PythonPackages forces copy too.
set(RequirementsFile "${SOURCE_DIRECTORY}/ThirdParty/PythonRequirements.txt")
set(PackagesStampFile "${BINARY_DIRECTORY}/CMakeFiles/PythonPackages.stamp")
set(Requirements "")
if(EXISTS "${RequirementsFile}")
    file(READ "${RequirementsFile}" Requirements)
endif()

set(CopiedRequirements "")
if(EXISTS "${PackagesStampFile}")
    file(READ "${PackagesStampFile}" CopiedRequirements)
endif()

if(NOT Requirements STREQUAL CopiedRequirements OR NOT EXISTS "${SOURCE_DIRECTORY}/Build/Data/PythonPackages")
    MirrorDirectory("${SOURCE_DIRECTORY}/ThirdParty/PythonPackages" "${SOURCE_DIRECTORY}/Build/Data/PythonPackages")
    file(WRITE "${PackagesStampFile}" "${Requirements}")
endif()