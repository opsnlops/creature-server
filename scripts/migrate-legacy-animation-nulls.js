// Removes oat++-era explicit null optionals from animation documents.
//
// Run a preview first:
//   mongosh "$CREATURE_MONGO_URI" --eval 'globalThis.dryRun = true' --file scripts/migrate-legacy-animation-nulls.js
//
// Apply only after reviewing the preview count and taking the normal MongoDB
// backup:
//   mongosh "$CREATURE_MONGO_URI" --eval 'globalThis.dryRun = false' --file scripts/migrate-legacy-animation-nulls.js
//
// The update is idempotent. It removes only legacy absence encodings and the
// obsolete metadata.last_updated field; it never alters required fields.

const dryRun = globalThis.dryRun ?? true;
if (typeof dryRun !== "boolean") {
    throw new Error("Set globalThis.dryRun to true or false before running this migration.");
}

const collection = db.getSiblingDB("creature_server").getCollection("animations");

const topLevelNullableFields = [
    "metadata.note",
    "metadata.source_script_id",
    "metadata.source_script_turns",
    "metadata.source_stage_id",
    "metadata.source_stage_updated_at",
    "metadata.source_stage_placements",
    "metadata.render_seed",
    "metadata.source_render_choices",
];

const filter = {
    $or: [
        ...topLevelNullableFields.map((field) => ({ [field]: { $type: "null" } })),
        { "tracks.creature_id": { $type: "null" } },
        { "tracks.fixture_id": { $type: "null" } },
        { "metadata.source_render_choices.idle_animation_id": { $type: "null" } },
        { "metadata.source_render_choices.idle_animation_id": "" },
        { "metadata.source_render_choices.idle_start_offset": { $type: "null" } },
        { "metadata.last_updated": { $exists: true } },
    ],
};

const matching = collection.countDocuments(filter);
if (dryRun) {
    printjson({ dry_run: true, matching_documents: matching });
} else {
    const updates = [];
    for (const field of topLevelNullableFields) {
        updates.push(collection.updateMany({ [field]: { $type: "null" } }, { $unset: { [field]: "" } }));
    }
    updates.push(collection.updateMany({ "metadata.last_updated": { $exists: true } }, { $unset: { "metadata.last_updated": "" } }));
    for (const field of ["creature_id", "fixture_id"]) {
        updates.push(
            collection.updateMany(
                { tracks: { $type: "array" }, [`tracks.${field}`]: { $type: "null" } },
                { $unset: { [`tracks.$[track].${field}`]: "" } },
                { arrayFilters: [{ [`track.${field}`]: { $type: "null" } }] },
            ),
        );
    }
    for (const field of ["idle_start_offset"]) {
        updates.push(
            collection.updateMany(
                {
                    "metadata.source_render_choices": { $type: "array" },
                    [`metadata.source_render_choices.${field}`]: { $type: "null" },
                },
                { $unset: { [`metadata.source_render_choices.$[choice].${field}`]: "" } },
                { arrayFilters: [{ [`choice.${field}`]: { $type: "null" } }] },
            ),
        );
    }
    updates.push(
        collection.updateMany(
            {
                "metadata.source_render_choices": { $type: "array" },
                "metadata.source_render_choices.idle_animation_id": { $in: [null, ""] },
            },
            { $unset: { "metadata.source_render_choices.$[choice].idle_animation_id": "" } },
            { arrayFilters: [{ "choice.idle_animation_id": { $in: [null, ""] } }] },
        ),
    );
    printjson({
        dry_run: false,
        matching_documents: matching,
        matched_updates: updates.reduce((total, result) => total + result.matchedCount, 0),
        modified_updates: updates.reduce((total, result) => total + result.modifiedCount, 0),
        remaining_documents: collection.countDocuments(filter),
    });
}
