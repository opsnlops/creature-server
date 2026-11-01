#include "server/transport/ApiDocumentation.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "TransportRouteManifest.h"
#include "Version.h"

namespace creatures::transport {
namespace {

using json = nlohmann::json;

std::string tagForController(std::string controller) {
    constexpr std::string_view suffix = "Controller";
    if (controller.size() >= suffix.size() && controller.ends_with(suffix)) {
        controller.resize(controller.size() - suffix.size());
    }
    return controller.empty() ? "Other" : controller;
}

std::string operationId(const json &route) {
    std::string result = route.at("controller").get<std::string>() + "." + route.at("handler").get<std::string>();
    result.front() = static_cast<char>(std::tolower(static_cast<unsigned char>(result.front())));
    return result;
}

json pathParameters(const std::string &path, const json &route) {
    json parameters = json::array();
    const json descriptions = route.value("path_params", json::object());
    std::size_t cursor = 0;
    while ((cursor = path.find('{', cursor)) != std::string::npos) {
        const auto end = path.find('}', cursor + 1);
        if (end == std::string::npos) {
            break;
        }
        const auto name = path.substr(cursor + 1, end - cursor - 1);
        json parameter = {{"name", name}, {"in", "path"}, {"required", true}, {"schema", {{"type", "string"}}}};
        if (descriptions.contains(name)) {
            parameter["description"] = descriptions.at(name);
        }
        parameters.push_back(std::move(parameter));
        cursor = end + 1;
    }
    const json queries = route.value("query_params", json::array());
    for (const auto &query : queries) {
        json parameter = {{"name", query.at("name")},
                          {"in", "query"},
                          {"required", query.value("required", false)},
                          {"schema", {{"type", "string"}}}};
        if (query.contains("default")) {
            parameter["schema"]["default"] = query.at("default");
        }
        if (query.contains("description")) {
            parameter["description"] = query.at("description");
        }
        parameters.push_back(std::move(parameter));
    }
    return parameters;
}

/// Manifest `path_params` notes that name a logical parameter rather than a
/// path segment (for example generation_id inside {filename}) still belong
/// in the operation description.
std::string describe(const std::string &path, const json &route) {
    std::string description = route.value("description", std::string{});
    // Bind before iterating: items() on a temporary would dangle.
    const json notes = route.value("path_params", json::object());
    for (const auto &[name, note] : notes.items()) {
        if (path.find("{" + name + "}") != std::string::npos) {
            continue;
        }
        if (!description.empty()) {
            description += "\n\n";
        }
        description += name + ": " + note.get<std::string>();
    }
    return description;
}

json responses(const json &route) {
    json result = json::object();
    const json declared = route.value("responses", json::object());
    for (const auto &[code, contentTypes] : declared.items()) {
        json content = json::object();
        for (const auto &contentType : contentTypes) {
            content[contentType.get<std::string>()] = json::object();
        }
        const int status = std::stoi(code);
        std::string description = status < 300 ? "Successful response" : status < 500 ? "Client error" : "Server error";
        if (status == 202) {
            description = "Accepted; a job id is returned and progress arrives over the WebSocket";
        } else if (status == 404) {
            description = "Not found";
        } else if (status == 413) {
            description = "Request body exceeds this route's limit";
        } else if (status == 429) {
            description = "Queue is full; try again shortly";
        } else if (status == 503) {
            description = "Server is saturated or shutting down";
        }
        result[code] = {{"description", description}, {"content", std::move(content)}};
    }
    if (result.empty()) {
        result["200"] = {{"description", "Successful response"}};
    }
    if (!result.contains("413") &&
        (route.at("method") == "POST" || route.at("method") == "PUT" || route.at("method") == "PATCH")) {
        result["413"] = {{"description", "Request body exceeds this route's limit"}};
    }
    if (!result.contains("503")) {
        result["503"] = {{"description", "Server is saturated or shutting down"}};
    }
    return result;
}

json buildOpenApiDocument() {
    const auto manifest = json::parse(generated::ROUTE_MANIFEST_JSON);
    json document = {{"openapi", "3.1.0"},
                     {"info",
                      {{"title", "Creature Server API"},
                       {"version", fmt::format("{}.{}.{}", CREATURE_SERVER_VERSION_MAJOR, CREATURE_SERVER_VERSION_MINOR,
                                               CREATURE_SERVER_VERSION_PATCH)},
                       {"description", "HTTP API for controlling April's animatronic creatures."}}},
                     {"servers", json::array({{{"url", "/"}, {"description", "This Creature Server"}}})},
                     {"paths", json::object()}};

    std::unordered_map<std::string, std::size_t> tagCounts;
    for (const auto &route : manifest.at("routes")) {
        const auto method = route.at("method").get<std::string>();
        if (method == "HEAD") {
            continue;
        }
        std::string normalizedMethod = method;
        std::transform(normalizedMethod.begin(), normalizedMethod.end(), normalizedMethod.begin(),
                       [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
        const auto path = route.at("path").get<std::string>();
        json tags = route.value("tags", json::array());
        if (tags.empty()) {
            tags.push_back(tagForController(route.at("controller").get<std::string>()));
        }
        for (const auto &tag : tags) {
            ++tagCounts[tag.get<std::string>()];
        }

        json operation = {{"operationId", operationId(route)},
                          {"summary", route.value("summary", route.at("handler").get<std::string>())},
                          {"tags", tags},
                          {"responses", responses(route)}};
        const auto description = describe(path, route);
        if (!description.empty()) {
            operation["description"] = description;
        }
        auto parameters = pathParameters(path, route);
        if (!parameters.empty()) {
            operation["parameters"] = std::move(parameters);
        }
        if (method == "POST" || method == "PUT" || method == "PATCH") {
            const json body = route.value("request_body", json::object());
            const auto contentType = body.value("content_type", std::string("application/json"));
            json schema = contentType == "application/json" ? json{{"type", "object"}, {"additionalProperties", true}}
                                                            : json{{"type", "string"}, {"format", "binary"}};
            operation["requestBody"] = {{"required", body.value("required", false)},
                                        {"content", {{contentType, {{"schema", std::move(schema)}}}}}};
        }
        document["paths"][path][normalizedMethod] = std::move(operation);
    }

    document["tags"] = json::array();
    for (const auto &[name, count] : tagCounts) {
        document["tags"].push_back({{"name", name}, {"description", fmt::format("{} route(s)", count)}});
    }
    std::sort(document["tags"].begin(), document["tags"].end(),
              [](const json &left, const json &right) { return left.at("name") < right.at("name"); });
    return document;
}

} // namespace

const std::string &openApiDocument() {
    static const std::string document = buildOpenApiDocument().dump(2);
    return document;
}

std::string_view apiBrowserHtml() {
    return R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Creature Server API</title>
  <style>
    :root{color-scheme:light dark;--bg:#101318;--panel:#191e26;--line:#313846;--text:#eef2f8;--muted:#9aa7b8;--accent:#67d1b8}
    *{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 ui-sans-serif,system-ui,sans-serif}
    header{position:sticky;top:0;z-index:2;padding:18px 24px;background:#101318ed;border-bottom:1px solid var(--line);backdrop-filter:blur(12px)}
    h1{font-size:22px;margin:0 0 4px}header p{margin:0;color:var(--muted)}main{max-width:1200px;margin:auto;padding:22px}
    input,textarea,button{font:inherit;color:inherit;background:#11161d;border:1px solid var(--line);border-radius:7px;padding:9px}
    #search{width:100%;margin-bottom:16px}section{margin:20px 0 30px}.tag{color:var(--accent);font-size:18px}
    details{background:var(--panel);border:1px solid var(--line);border-radius:9px;margin:8px 0;overflow:hidden}
    summary{display:flex;gap:12px;align-items:center;cursor:pointer;padding:12px}.method{width:64px;font-weight:800;color:#101318;text-align:center;border-radius:5px;padding:3px}
    .GET{background:#67d1b8}.POST{background:#ffd166}.PUT{background:#8ab4f8}.DELETE{background:#ff7b86}.PATCH{background:#c59bf6}
    code{font-size:14px}.summary{margin-left:auto;color:var(--muted)}.workbench{border-top:1px solid var(--line);padding:14px}
    .params{display:grid;grid-template-columns:160px 1fr;gap:8px;align-items:center}.params input{width:100%}textarea{width:100%;min-height:100px;margin-top:10px;font-family:ui-monospace,monospace}
    button{margin-top:10px;background:var(--accent);border:0;color:#071310;font-weight:800;cursor:pointer}.result{white-space:pre-wrap;overflow:auto;max-height:420px;background:#0b0e12;padding:12px;border-radius:7px;margin-top:10px}.hidden{display:none}
    .description{margin:0 0 10px;color:var(--text);white-space:pre-wrap}.responses{margin:0 0 12px;color:var(--muted);font-size:13px}
    a{color:var(--accent)}@media(max-width:650px){.summary{display:none}.params{grid-template-columns:1fr}main{padding:12px}}
  </style>
</head>
<body>
<header><h1>Creature Server API</h1><p>Local API browser · <a href="/api/openapi.json">OpenAPI JSON</a></p></header>
<main><input id="search" type="search" placeholder="Filter by method, path, or operation…" aria-label="Filter routes"><div id="routes">Loading API…</div></main>
<script>
const methods=['get','post','put','delete','patch'];
const el=(name,cls,text)=>{const n=document.createElement(name);if(cls)n.className=cls;if(text!==undefined)n.textContent=text;return n};
function endpoint(path,method,op){
  const box=el('details','endpoint');box.dataset.search=(method+' '+path+' '+(op.summary||'')).toLowerCase();
  const head=el('summary');head.append(el('span','method '+method.toUpperCase(),method.toUpperCase()));head.append(el('code','',path));head.append(el('span','summary',op.summary||op.operationId));box.append(head);
  const work=el('div','workbench'),params=el('div','params');
  if(op.description){work.append(el('p','description',op.description))}
  const codes=Object.entries(op.responses||{}).map(([c,r])=>c+' '+(r.description||'')+(r.content?' ('+Object.keys(r.content).join(', ')+')':'')).join(' · ');if(codes)work.append(el('p','responses',codes));
  for(const p of op.parameters||[]){const label=el('label','',p.name+(p.in==='query'?' (query)':''));if(p.description)label.title=p.description;const input=el('input');input.dataset.param=p.name;input.dataset.where=p.in;input.placeholder=p.description||p.in;if(p.schema&&p.schema.default!==undefined)input.value=p.schema.default;params.append(label,input)}work.append(params);
  let body;const bodyType=op.requestBody&&Object.keys(op.requestBody.content||{})[0];const jsonBody=!bodyType||bodyType==='application/json';
  if(['post','put','patch'].includes(method)){body=el('textarea');body.placeholder=jsonBody?('JSON request body'+(op.requestBody&&op.requestBody.required?' (required)':' (optional)')):('This route takes a raw '+bodyType+' body; send it with a client, not from here');if(!jsonBody)body.disabled=true;work.append(body)}
  const send=el('button','',method==='get'?'Send request':'Send '+method.toUpperCase()+' request');const result=el('pre','result hidden');
  send.onclick=async()=>{let url=path;const query=new URLSearchParams();for(const input of params.querySelectorAll('input')){if(input.dataset.where==='query'){if(input.value!=='')query.append(input.dataset.param,input.value)}else{url=url.replace('{'+input.dataset.param+'}',encodeURIComponent(input.value))}}if([...query].length)url+='?'+query;result.classList.remove('hidden');result.textContent='Loading…';
    const init={method:method.toUpperCase(),headers:{Accept:'application/json'}};if(body&&!body.disabled&&body.value.trim()){init.headers['Content-Type']='application/json';init.body=body.value}
    try{const response=await fetch(url,init),text=await response.text();let pretty=text;try{pretty=JSON.stringify(JSON.parse(text),null,2)}catch{}result.textContent=response.status+' '+response.statusText+'\n\n'+pretty}catch(error){result.textContent='Request failed: '+error}}
  work.append(send,result);box.append(work);return box;
}
fetch('/api/openapi.json').then(r=>{if(!r.ok)throw Error(r.status+' '+r.statusText);return r.json()}).then(spec=>{
  const root=document.getElementById('routes');root.textContent='';const groups=new Map();for(const [path,item] of Object.entries(spec.paths)){for(const method of methods){if(!item[method])continue;const op=item[method],tag=(op.tags||['Other'])[0];if(!groups.has(tag))groups.set(tag,[]);groups.get(tag).push(endpoint(path,method,item[method]))}}
  for(const tag of [...groups.keys()].sort()){const section=el('section'),title=el('h2','tag',tag);section.append(title,...groups.get(tag));root.append(section)}
  document.getElementById('search').oninput=e=>{const q=e.target.value.toLowerCase();for(const route of document.querySelectorAll('.endpoint'))route.hidden=!route.dataset.search.includes(q)};
}).catch(error=>{document.getElementById('routes').textContent='Unable to load the API catalog: '+error});
</script>
</body></html>)HTML";
}

} // namespace creatures::transport
