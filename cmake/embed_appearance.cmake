get_filename_component(resource_directory "${output}" DIRECTORY)
file(MAKE_DIRECTORY "${resource_directory}")
set(hash_file "${resource_directory}/appearance.sha256")
file(SHA256 "${component}" digest)
file(WRITE "${hash_file}" "${digest}")
file(WRITE "${output}"
    "#pragma code_page(65001)\n#include \"resource.h\"\n"
    "IDR_APPEARANCE_MODULE RCDATA \"${component}\"\n"
    "IDR_APPEARANCE_HASH RCDATA \"${hash_file}\"\n"
    "IDR_THIRD_PARTY_NOTICES RCDATA \"${notices}\"\n")
