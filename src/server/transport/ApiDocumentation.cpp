#include "server/transport/ApiDocumentation.h"

#include <algorithm>
#include <cctype>
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

json pathParameters(const std::string &path) {
    json parameters = json::array();
    std::size_t cursor = 0;
    while ((cursor = path.find('{', cursor)) != std::string::npos) {
        const auto end = path.find('}', cursor + 1);
        if (end == std::string::npos) {
            break;
        }
        const auto name = path.substr(cursor + 1, end - cursor - 1);
        parameters.push_back({{"name", name}, {"in", "path"}, {"required", true}, {"schema", {{"type", "string"}}}});
        cursor = end + 1;
    }
    return parameters;
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
        const auto tag = tagForController(route.at("controller").get<std::string>());
        ++tagCounts[tag];

        json operation = {
            {"operationId", operationId(route)},
            {"summary", route.at("handler")},
            {"tags", json::array({tag})},
            {"responses",
             {{"200", {{"description", "Successful response"}}}, {"default", {{"description", "Error response"}}}}}};
        auto parameters = pathParameters(path);
        if (!parameters.empty()) {
            operation["parameters"] = std::move(parameters);
        }
        if (method == "POST" || method == "PUT" || method == "PATCH") {
            operation["requestBody"] = {
                {"required", false},
                {"content",
                 {{"application/json", {{"schema", {{"type", "object"}, {"additionalProperties", true}}}}}}}};
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
  for(const p of op.parameters||[]){const label=el('label','',p.name);const input=el('input');input.dataset.param=p.name;input.placeholder=p.in;params.append(label,input)}work.append(params);
  let body;if(['post','put','patch'].includes(method)){body=el('textarea');body.placeholder='JSON request body (optional)';work.append(body)}
  const send=el('button','',method==='get'?'Send request':'Send '+method.toUpperCase()+' request');const result=el('pre','result hidden');
  send.onclick=async()=>{let url=path;for(const input of params.querySelectorAll('input'))url=url.replace('{'+input.dataset.param+'}',encodeURIComponent(input.value));result.classList.remove('hidden');result.textContent='Loading…';
    const init={method:method.toUpperCase(),headers:{Accept:'application/json'}};if(body&&body.value.trim()){init.headers['Content-Type']='application/json';init.body=body.value}
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
