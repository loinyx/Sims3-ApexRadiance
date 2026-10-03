import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { spawn, spawnSync } from 'node:child_process';

// Generic execution helper: project instructions supply the recipe, never guessed commands.
const options = new Map();
const mode = process.argv[2];
for (let i = 3; i < process.argv.length; i += 2) {
  if (!process.argv[i].startsWith('--') || !process.argv[i + 1]) throw Error('Expected --key value');
  options.set(process.argv[i].slice(2), process.argv[i + 1]);
}
const repo = fs.realpathSync(options.get('repo') || process.cwd());
const normalized = process.platform === 'win32' ? repo.toLowerCase() : repo;
const key = crypto.createHash('sha256').update(normalized).digest('hex').slice(0, 20);
const skill = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const cache = path.join(skill, 'cache', key);
const recipePath = path.join(cache, 'recipe.json');
const known = /^(package\.json|.*lock.*|CMakeLists\.txt|CMakePresets\.json|Cargo\.toml|Cargo\.lock|go\.mod|go\.sum|Makefile|pom\.xml|build\.gradle.*|settings\.gradle.*|.*\.(sln|vcxproj|csproj|props|targets)|pyproject\.toml|meson\.build|vcpkg\.json)$/;
function manifests() {
  return Object.fromEntries(fs.readdirSync(repo).filter(n => known.test(n) && fs.statSync(path.join(repo,n)).isFile())
    .sort().map(n => [n, crypto.createHash('sha256').update(fs.readFileSync(path.join(repo,n))).digest('hex')]));
}
function prerequisites(recipe) {
  if (!recipe || typeof recipe.executable !== 'string' || !recipe.executable.trim() ||
      !Array.isArray(recipe.args) || !recipe.args.every(a => typeof a === 'string') ||
      !Array.isArray(recipe.artifacts) || !recipe.artifacts.every(a=>typeof a==='string') ||
      (recipe.requires && (!Array.isArray(recipe.requires) || !recipe.requires.every(a=>typeof a==='string'))))
    throw Error('Invalid recipe: executable, literal args[], artifacts[] and optional requires[] expected');
  if (/\.(bat|cmd)$/i.test(recipe.executable)) throw Error('Use an explicit cmd.exe invocation for a reviewed .cmd/.bat script');
  return (recipe.requires || []).filter(p => !fs.existsSync(path.resolve(repo,p)));
}
let cached;
if (fs.existsSync(recipePath)) cached=JSON.parse(fs.readFileSync(recipePath,'utf8'));
const stamps=manifests();
const cacheValid = cached && cached.repo === repo && JSON.stringify(cached.manifests)===JSON.stringify(stamps) &&
  prerequisites(cached.recipe).length === 0;
if (mode === 'inspect') {
  const tools = ['node','npm','pnpm','cmake','ninja','cargo','go','dotnet','msbuild','make','mvn','gradle'];
  const found = Object.fromEntries(tools.map(tool => {
    const r=spawnSync(process.platform==='win32'?'where.exe':'which',[tool],{encoding:'utf8',windowsHide:true});
    return [tool,r.status===0?r.stdout.trim().split(/\r?\n/)[0]:null];
  }).filter(([,v])=>v));
  console.log(JSON.stringify({repo,manifests:Object.keys(stamps),tools:found,cacheValid:!!cacheValid,
    recipe:cached?.recipe,cachePath:recipePath},null,2));
} else if (mode === 'run') {
  let recipe;
  if(options.has('recipe')) recipe=JSON.parse(fs.readFileSync(options.get('recipe'),'utf8').replace(/^\uFEFF/,''));
  else if(cacheValid)recipe=cached.recipe;
  else throw Error('No valid cached recipe; inspect the project and supply --recipe JSON');
  if(options.has('target') && recipe.target!==options.get('target'))throw Error('Recipe target differs from requested target');
  const missing=prerequisites(recipe);
  if(missing.length)throw Error('Missing prerequisite: '+missing.join(', '));
  fs.mkdirSync(cache,{recursive:true});
  const logPath=path.join(cache,'build-'+new Date().toISOString().replace(/[:.]/g,'-')+'.log');
  const log=fs.createWriteStream(logPath);
  let tail='',warnings=0;
  const started=Date.now();
  const child=spawn(recipe.executable,recipe.args,{cwd:repo,shell:false,windowsHide:true,stdio:['ignore','pipe','pipe']});
  const consume=data=>{log.write(data);const s=data.toString();tail=(tail+s).slice(-12000);warnings+=(s.match(/\bwarning\b/gi)||[]).length;};
  child.stdout.on('data',consume);child.stderr.on('data',consume);
  const result=await new Promise(resolve=>{
    child.once('error',error=>resolve({exitCode:null,error:error.message}));
    child.once('close',(code,signal)=>resolve({exitCode:code,signal}));
  });
  await new Promise(resolve=>log.end(resolve));
  const artifacts=recipe.artifacts.map(p=>{
    const absolute=path.resolve(repo,p);let st;
    try{st=fs.statSync(absolute);}catch{}
    return {path:absolute,bytes:st?.isFile()?st.size:0,modified:st?.mtime.toISOString()};
  });
  const success=result.exitCode===0 && artifacts.every(a=>a.bytes>0);
  if(success)fs.writeFileSync(recipePath,JSON.stringify({repo,manifests:manifests(),recipe,validatedAt:new Date().toISOString()},null,2));
  console.log(JSON.stringify({success,...result,target:recipe.target||null,seconds:+((Date.now()-started)/1000).toFixed(2),warnings,
    artifacts,log:logPath,recipeSaved:success},null,2));
  if(!success)console.log(tail.split(/\r?\n/).slice(-25).join('\n'));
  process.exitCode=success?0:1;
} else throw Error('Usage: build.mjs inspect|run --repo path [--recipe JSON] [--target name]');
