# Database Method Observability Standard

The conventions every `Database::*` method should follow so Honeycomb traces are queryable, errors are filterable, and the trace structure is uniform across resource families. Established in issue #17. Applies to MongoDB-backed methods in `src/server/{creature,animation,playlist,fixture,script}/`.

> Sweep status: script ✅ fixture ✅ playlist ✅ animation ✅ creature ✅. All `Database::*` methods conform as of the issue #17 closure. New methods land conformant; check the reviewer checklist at the bottom of this doc.

---

## Canonical method signature

```cpp
Result<T> Database::doSomething(const id_t &id,
                                const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
```

- Take the parent span as **`const std::shared_ptr<OperationSpan> &`** with a `nullptr` default. (Older methods sometimes use by-value or no default — bring them to this form when you touch them.)
- The first child span the method creates uses the name `Database.<methodName>` (e.g. `Database.getFixture`, `Database.upsertDialogScript`).

---

## Required attributes on the root op span

Every DB method's root op span MUST set:

```cpp
auto span = creatures::observability->createChildOperationSpan("Database.<methodName>", parentSpan);
if (span) {
    span->setAttribute("database.collection", FOO_COLLECTION);   // e.g. "fixtures"
    span->setAttribute("database.operation",  "find_one");        // see table below
    span->setAttribute("database.system",     "mongodb");
    span->setAttribute("database.name",       DB_NAME);
    // ...and the resource id if one's in scope:
    span->setAttribute("fixture.id", fixtureId);                  // <resource>.id
}
```

`database.operation` follows OpenTelemetry semantic conventions:

| Method shape | `database.operation` |
|---|---|
| `getXById` / `getXJson` | `"find_one"` |
| `listX` / `getAllX` | `"find"` |
| `upsertX` | `"update_one"` (with `upsert=true`) |
| `deleteX` | `"delete_one"` |
| `setXField` (partial update) | `"update_one"` |
| `insertX` (no overwrite — e.g. ad-hoc TTL collection) | `"insert_one"` |

---

## Required child-span structure

Wrap the actual Mongo call in a dedicated child span so network time is separable from BSON/JSON conversion time in Honeycomb.

```cpp
auto mongoSpan = creatures::observability->createChildOperationSpan("<methodName>.mongoQuery", span);
auto maybe = collection.find_one(filter.view());
mongoSpan->setSuccess();
```

Child span names follow the pattern `<methodName>.<sub-step>`. Conventional sub-step names:

- `<methodName>.parse-json` — parsing the incoming JSON string into a `json` object
- `<methodName>.json-to-bson` — converting to BSON for storage
- `<methodName>.get-collection` — fetching the Mongo collection handle (failures here surface DB connection issues)
- `<methodName>.mongoQuery` — the Mongo network call (`find_one`, `find`, `update_one`, `delete_one`, `insert_one`)
- `<methodName>.bson-to-json` / `<methodName>.json::parse` — converting the BSON reply back

Not every method needs every sub-step. The non-negotiable one is **wrap the Mongo call**.

---

## Required success attributes (by method category)

On the root span, after the operation succeeds:

**Reads (get-one):**
```cpp
span->setAttribute("db.response_size_bytes", static_cast<int64_t>(json.dump().length()));
```

**Lists:**
```cpp
span->setAttribute("<resources>.count", static_cast<int64_t>(items.size()));   // e.g. "fixtures.count"
```

**Upserts:**
```cpp
span->setAttribute("<resource>.id", upserted.id);
// Domain-relevant counts/flags — what would an oncall want to filter on?
span->setAttribute("fixture.channels_count", static_cast<int64_t>(fixture.channels.size()));
```

**Deletes:**
- Just `<resource>.id` (already set on entry).

Always end with `span->setSuccess();` on the success path.

---

## Required error envelope

Every error path needs the same three pieces:

```cpp
if (!result.isSuccess()) {
    auto err = result.getError().value();
    if (span) {
        span->setError(err.getMessage());
        span->setAttribute("error.type", "InvalidData");                       // see categories below
        span->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
    }
    return Result<T>{err};
}
```

For caught exceptions:

```cpp
} catch (const mongocxx::exception &e) {
    std::string msg = fmt::format("MongoDB error in <methodName>: {}", e.what());
    error(msg);
    if (span) {
        span->recordException(e);                                              // attaches name + message
        span->setError(msg);
        span->setAttribute("error.type", "MongoDBException");
        span->setAttribute("error.code", static_cast<int64_t>(ServerError::DatabaseError));
    }
    return Result<T>{ServerError(ServerError::DatabaseError, msg)};
}
```

`error.type` values used across the codebase (use these, don't invent new ones):

- `"InvalidData"` — client-provided JSON failed validation
- `"NotFound"` — Mongo returned nothing for a single-item lookup
- `"DatabaseError"` — getCollection failed, connection issue
- `"MongoDBException"` — caught a `mongocxx::exception`
- `"DataFormatException"` — caught a `DataFormatException` from JsonParser
- `"std::exception"` — caught a generic `std::exception`
- `"JsonParsingException"` — caught a `nlohmann::json::exception`

---

## "No parent span" warning

If a method is reached with a null `parentSpan`, log a warn so the missing context is visible:

```cpp
if (!parentSpan) {
    warn("no parent span provided for Database.<methodName>, creating a root span");
}
```

The `createChildOperationSpan` call works with nullptr (treats it as a root), but this warn is the canary that some caller forgot to thread a span.

---

## Canonical example: a read

```cpp
Result<json> Database::getFooJson(const fooId_t &fooId,
                                  const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getFooJson, creating a root span");
    }

    auto span = creatures::observability->createChildOperationSpan("Database.getFooJson", parentSpan);
    if (span) {
        span->setAttribute("database.collection", FOOS_COLLECTION);
        span->setAttribute("database.operation", "find_one");
        span->setAttribute("database.system", "mongodb");
        span->setAttribute("database.name", DB_NAME);
        span->setAttribute("foo.id", fooId);
    }

    if (fooId.empty()) {
        std::string msg = "getFooJson called with empty fooId";
        warn(msg);
        if (span) {
            span->setError(msg);
            span->setAttribute("error.type", "InvalidData");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
        return Result<json>{ServerError(ServerError::InvalidData, msg)};
    }

    auto collectionResult = getCollection(FOOS_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        if (span) {
            span->setError(err.getMessage());
            span->setAttribute("error.type", "DatabaseError");
            span->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        return Result<json>{err};
    }
    auto collection = collectionResult.getValue().value();

    std::shared_ptr<OperationSpan> mongoSpan;
    try {
        mongoSpan = creatures::observability->createChildOperationSpan("getFooJson.mongoQuery", span);
        auto filter = bsoncxx::builder::stream::document{} << "id" << fooId
                      << bsoncxx::builder::stream::finalize;
        auto maybe = collection.find_one(filter.view());
        mongoSpan->setSuccess();

        if (!maybe) {
            std::string msg = fmt::format("Foo not found: {}", fooId);
            warn(msg);
            if (span) {
                span->setError(msg);
                span->setAttribute("error.type", "NotFound");
                span->setAttribute("error.code", static_cast<int64_t>(ServerError::NotFound));
            }
            return Result<json>{ServerError(ServerError::NotFound, msg)};
        }

        auto convertSpan = creatures::observability->createChildOperationSpan("getFooJson.bson-to-json", span);
        auto jsonResult = JsonParser::bsonToJson(maybe->view(), fmt::format("foo {}", fooId), convertSpan);
        if (!jsonResult.isSuccess()) {
            return jsonResult;
        }
        json j = jsonResult.getValue().value();

        if (span) {
            span->setAttribute("db.response_size_bytes", static_cast<int64_t>(j.dump().length()));
            span->setSuccess();
        }
        return Result<json>{j};

    } catch (const mongocxx::exception &e) {
        std::string msg = fmt::format("MongoDB exception in getFooJson({}): {}", fooId, e.what());
        error(msg);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(msg);
        }
        if (span) {
            span->recordException(e);
            span->setError(msg);
            span->setAttribute("error.type", "MongoDBException");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::DatabaseError));
        }
        return Result<json>{ServerError(ServerError::DatabaseError, msg)};
    } catch (const std::exception &e) {
        std::string msg = fmt::format("Exception in getFooJson({}): {}", fooId, e.what());
        error(msg);
        if (span) {
            span->recordException(e);
            span->setError(msg);
            span->setAttribute("error.type", "std::exception");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
        }
        return Result<json>{ServerError(ServerError::InternalError, msg)};
    }
}
```

The fixture and script families' `getXJson` methods are the closest reference impls — copy from those.

---

## Reviewer checklist

Use this when reviewing a new or modified DB method:

- [ ] Signature: `const std::shared_ptr<OperationSpan> &parentSpan = nullptr`
- [ ] "no parent span" warn at the top
- [ ] Root span named `Database.<methodName>`
- [ ] Root span has all 4 `database.*` attributes
- [ ] Root span has `<resource>.id` (or equivalent) when in scope
- [ ] Mongo call wrapped in `<methodName>.mongoQuery` child span
- [ ] `setSuccess()` on root span on success
- [ ] Read methods set `db.response_size_bytes` on success
- [ ] List methods set `<resources>.count` on success
- [ ] Upsert methods set the new id + meaningful per-resource counts/flags
- [ ] Every error path: `setError(msg)` + `setAttribute("error.type", ...)` + `setAttribute("error.code", ...)`
- [ ] Caught exceptions: `recordException(e)` + the error envelope above
- [ ] Uses a value from the documented `error.type` vocabulary (don't invent new ones)

---

## Why these specific attributes

- **`database.collection` / `database.operation`** — lets you slice Honeycomb queries by which collection is slow, or what kind of operation: `WHERE database.collection = "animations" GROUP BY database.operation`.
- **`db.response_size_bytes`** — answers "is this query slow because the doc is huge?" (large animations).
- **`<resources>.count`** — answers "is this list slow because there are 10,000 items?".
- **`error.type` / `error.code`** — the difference between "show me all errors" (works with just `setError`) and "show me all `NotFound`s on the playlist collection" (needs the type attribute as a filter).
- **`recordException`** — captures the C++ exception class name + message as standard OTel exception event, queryable separately from the human-readable `error.message`.
