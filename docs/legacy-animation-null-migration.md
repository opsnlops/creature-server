# Legacy animation null cleanup

Before the neutral JSON model work, oat++ serialized absent optional values as
explicit JSON `null`. Those values are valid legacy persistence data but are no
longer the canonical animation representation: new API clients omit absent
options and new animation writes do the same.

The server tolerates these legacy nulls only while reading persisted animation
documents. It continues to reject explicit nulls from API input.

Use the migration script to remove legacy nulls from existing documents after
taking the normal MongoDB backup. It is idempotent and previews by default:

```sh
mongosh "$CREATURE_MONGO_URI" --eval 'globalThis.dryRun = true' \
  --file scripts/migrate-legacy-animation-nulls.js
```

Review `matching_documents`, then apply:

```sh
mongosh "$CREATURE_MONGO_URI" --eval 'globalThis.dryRun = false' \
  --file scripts/migrate-legacy-animation-nulls.js
```

The script affects only `creature_server.animations`. It removes legacy null
optionals, the legacy Console-only `metadata.last_updated` field, and empty
`idle_animation_id` values (which mean no idle loop). It does not rewrite
populated values or remove required fields.
