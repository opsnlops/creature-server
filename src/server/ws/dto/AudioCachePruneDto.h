#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

/// Result of an audio-cache prune pass (issue #166).
///
/// The Opus cache is keyed by a hash of a source file's canonical path, so
/// moving or deleting a sound orphans its cache entry permanently — nothing
/// else in the system reclaims it. This is the report from the explicit
/// maintenance operation that does, intended to be wrapped by creature-cli.
class AudioCachePruneDto : public oatpp::DTO {

    DTO_INIT(AudioCachePruneDto, DTO)

    DTO_FIELD_INFO(dry_run) {
        info->description = "True when nothing was deleted and the counts describe what WOULD be removed. "
                            "Defaults to true — pass dry_run=false to actually delete.";
    }
    DTO_FIELD(Boolean, dry_run);

    DTO_FIELD_INFO(entries_scanned) { info->description = "Cache entries (one per cached source file) examined."; }
    DTO_FIELD(UInt64, entries_scanned);

    DTO_FIELD_INFO(orphaned_entries) {
        info->description = "Entries whose recorded source file no longer exists — typically a sound that was moved "
                            "or deleted.";
    }
    DTO_FIELD(UInt64, orphaned_entries);

    DTO_FIELD_INFO(incomplete_entries) {
        info->description = "Entries with no completion marker or unreadable metadata: a crashed or partial save. "
                            "These can never be read back, since a cache hit requires the marker.";
    }
    DTO_FIELD(UInt64, incomplete_entries);

    DTO_FIELD_INFO(temporary_files) {
        info->description = "Abandoned *.tmp.<pid>.<n> files left by a save that died before its rename.";
    }
    DTO_FIELD(UInt64, temporary_files);

    DTO_FIELD_INFO(orphaned_lock_files) {
        info->description = "Advisory .lock files whose cache directory is gone. Locks are siblings of the directory "
                            "by design, so a clear leaves them behind.";
    }
    DTO_FIELD(UInt64, orphaned_lock_files);

    DTO_FIELD_INFO(bytes_reclaimed) {
        info->description = "Bytes freed (or that would be freed, when dry_run is true).";
    }
    DTO_FIELD(UInt64, bytes_reclaimed);

    DTO_FIELD_INFO(removed) {
        info->description = "Bounded sample (at most 100) of the paths removed, for operator inspection.";
    }
    DTO_FIELD(List<String>, removed);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
