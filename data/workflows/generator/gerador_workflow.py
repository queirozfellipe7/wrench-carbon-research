"""
gerador_workflow.py — Gerador de workflows sintéticos para avaliação de schedulers
heterogêneos (HEFT, DLS, PEFT, IPEFT, MOHEFT).

MUDANÇA DE FUNDO EM RELAÇÃO À VERSÃO ANTERIOR:
  A versão anterior encadeava blocos PolyBench inteiros em `platform_size *
  oversub_factor` LANES PARALELAS INDEPENDENTES, sem nenhuma aresta cruzando
  lanes. Isso produz um DAG "fork-join raso" onde não existe caminho crítico
  não-trivial nem razão comunicação/computação (CCR) controlada — ou seja,
  não existe a decisão difícil que faz HEFT, DLS, PEFT e IPEFT divergirem.
  Resultado: todo scheduler "acerta igual", sem carga científica.

  Esta versão gera a TOPOLOGIA como um DAG em níveis, no estilo do gerador
  clássico usado nos papers de referência do projeto (Topcuoglu/Wu — HEFT;
  Arabnejad/Barbosa — PEFT; Zhou et al. — IPEFT; que por sua vez usam o
  gerador de Suter, "daggen"), com os parâmetros clássicos:

    - fat        : largura média por nível (fat * sqrt(n))
    - density    : probabilidade de aresta entre um nó e um "pai" candidato
    - regularity : uniformidade da largura entre níveis (1.0 = todos iguais)
    - jump       : distância máxima de nível para trás que uma aresta pode cruzar
    - ccr        : razão comunicação/computação alvo (soma custo de arestas /
                   soma custo de nós), controlada de fato — não é mais um
                   tamanho de arquivo aleatório desacoplado do custo da tarefa.

  O custo computacional de cada tarefa (flops/memory) continua vindo de uma
  distribuição amostrada empiricamente dos blocos PolyBench reais (realismo
  de carga), mas agora seguindo a formulação clássica dos papers: escolhe-se
  wDAG (custo médio do "grafo") e cada tarefa recebe wi ~ Uniforme(0, 2*wDAG).

  Heterogeneidade de processador (β dos papers) NÃO é sintetizada aqui: ela
  já existe fisicamente nos platform_Ngpu.xml (GPU1-4 a 67000Gf vs GPU5-8 a
  19500Gf, ~3.4x) e emerge automaticamente porque o WRENCH calcula tempo de
  execução como flops/velocidade_do_host. Sintetizar uma matriz W (v x p)
  adicional seria redundante e poderia mascarar essa heterogeneidade real.

  O schema JSON de saída (jobs/files/dependencies) é IDÊNTICO ao anterior —
  nenhuma mudança é necessária no Controller.cpp de nenhum dos 5 schedulers.
"""

import json
import os
import random
import copy
import argparse
import math
import statistics
import collections

# ==============================
# ÂNCORA DE CAMINHOS — resolve os defaults relativos à LOCALIZAÇÃO do
# script (não ao diretório de onde ele é chamado).
# Estrutura esperada: <PROJECT_ROOT>/data/workflows/generator/gerador_workflow.py
# ==============================
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", "..", ".."))
DEFAULT_BASE_FOLDER = os.path.join(PROJECT_ROOT, "data", "workflows", "generator", "polybench_base")
DEFAULT_WORKFLOWS_ROOT = os.path.join(PROJECT_ROOT, "data", "workflows")

# ==============================
# CALIBRAÇÃO PARA CONVERTER FLOPS <-> BYTES NO CÁLCULO DE CCR
# ==============================
# CCR é definido nos papers como (soma dos custos de aresta) / (soma dos
# custos de nó), ambos em TEMPO. Como nós carregam custo em FLOPs e arestas
# em bytes, precisamos de uma velocidade e uma banda de referência para
# converter os dois para tempo e então calibrar o tamanho dos arquivos.
# Ajuste estas constantes se os valores reais do seu platform.xml mudarem
# muito (velocidade média de GPU e banda do storage_service).
REFERENCE_GFLOPS = 40000.0          # ~média entre GPU1-4 (67000) e GPU5-8 (19500)
REFERENCE_FLOPS_PER_SEC = REFERENCE_GFLOPS * 1e9  # Gf -> FLOPs/s

# Banda REAL do único disco físico do projeto (nvme_storage, só existe em
# DGX_System — GPUs não têm storage próprio). Toda comunicação do workflow
# passa por ESTE recurso único e serializado, então usamos a banda combinada
# real (escrita seguida de leitura), não um valor médio fictício.
DISK_WRITE_BPS = 12e9
DISK_READ_BPS = 25e9
# tempo por byte transferido = tempo_escrita + tempo_leitura (sequenciais,
# porque a dependência é parent -> write -> read -> child)
REFERENCE_BANDWIDTH_BPS = 1.0 / (1.0 / DISK_WRITE_BPS + 1.0 / DISK_READ_BPS)

FLOPS_TARGET_MIN = 1e9
FLOPS_TARGET_MAX = 1e13
MEM_TARGET_MIN = 1e6
MEM_TARGET_MAX = 1e10
FILE_TARGET_MIN = 1e4
FILE_TARGET_MAX = 1e9


def log_normalize(value, target_min, target_max):
    if value <= 0:
        return int(target_min)
    log_val = math.log10(value)
    log_min = math.log10(target_min)
    log_max = math.log10(target_max)
    clamped = max(log_min, min(log_max, log_val))
    return int(10 ** clamped)


# ==============================
# 1. POOL DE CUSTO REAL (PolyBench) — só para realismo de carga
# ==============================
def load_polybench_cost_pool(folder):
    """
    Varre recursivamente `folder` e retorna duas listas: flops observados e
    memory observados nos blocos PolyBench reais, já normalizados na escala
    alvo. Usado só para amostrar wDAG (custo médio "realista"), não a
    topologia (que agora é sintética/parametrizada).
    """
    flops_pool, mem_pool = [], []
    for dirpath, _, filenames in os.walk(folder):
        for fname in filenames:
            if not fname.endswith(".json") or fname.startswith(("wf_", "large_")):
                continue
            with open(os.path.join(dirpath, fname), "r") as fh:
                wf = json.load(fh)
            for job in wf.get("jobs", []):
                if "flops" in job:
                    flops_pool.append(log_normalize(job["flops"], FLOPS_TARGET_MIN, FLOPS_TARGET_MAX))
                if "memory" in job:
                    mem_pool.append(log_normalize(job["memory"], MEM_TARGET_MIN, MEM_TARGET_MAX))
            for f in wf.get("files", []):
                if "size" in f:
                    pass  # tamanho de arquivo real não é usado: CCR agora é calibrado, não herdado

    if not flops_pool:
        raise ValueError(
            f"Nenhum custo de tarefa PolyBench encontrado em: {folder}.\n"
            "Verifique se a pasta contém blocos .json válidos (com campo 'flops')."
        )
    return flops_pool, (mem_pool or [MEM_TARGET_MIN])


# ==============================
# 2. TOPOLOGIA EM NÍVEIS (fat / density / regularity / jump)
# ==============================
def generate_layered_topology(n, fat, density, regularity, jump, rng):
    """
    Gera a estrutura de um DAG (níveis + arestas) seguindo o modelo clássico
    usado em HEFT/PEFT/IPEFT (adaptado do gerador de Suter/daggen).

    Retorna:
      levels: lista de níveis, cada um é uma lista de índices de tarefa
      edges:  lista de tuplas (idx_pai, idx_filho)
    """
    mean_width = max(1, round(fat * math.sqrt(n)))
    # regularity baixo -> larguras bem dispersas; regularity alto -> larguras uniformes
    spread = max(0.0, 1.0 - regularity)

    levels = []
    assigned = 0
    while assigned < n:
        if spread == 0.0:
            width = mean_width
        else:
            low = max(1, int(mean_width * (1 - spread)))
            high = max(low, int(mean_width * (1 + spread)))
            width = rng.randint(low, high)
        width = min(width, n - assigned)
        level_ids = list(range(assigned, assigned + width))
        levels.append(level_ids)
        assigned += width

    # mapa índice de tarefa -> nível
    task_level = {}
    for lvl_idx, ids in enumerate(levels):
        for tid in ids:
            task_level[tid] = lvl_idx

    edges = []
    # Teto de candidatos avaliados por nó, INDEPENDENTE de n. Sem isso, a
    # largura média (fat*sqrt(n)) faz o nº de candidatos por nó crescer com
    # sqrt(n) — e como `density` é uma probabilidade por candidato, o total
    # de arestas explode como O(n^1.5) em vez de O(n): num workflow de 256
    # tasks a proporção I/O:compute fica ~14x, mas em 2048 tasks (mesmos
    # fat/density/jump) vira ~44x, só por causa desse crescimento espúrio.
    # Cap = jump * 8 (equivalente a ~8 candidatos "efetivos" por nível
    # retroativo, próximo da largura média típica usada nos papers p/ n~256).
    max_candidates_per_node = max(4, jump * 8)

    for lvl_idx in range(1, len(levels)):
        for child in levels[lvl_idx]:
            candidate_parents = []
            for back in range(1, jump + 1):
                src_lvl = lvl_idx - back
                if src_lvl >= 0:
                    candidate_parents.extend(levels[src_lvl])
            if not candidate_parents:
                continue

            if len(candidate_parents) > max_candidates_per_node:
                candidate_parents = rng.sample(candidate_parents, max_candidates_per_node)

            connected = False
            for parent in candidate_parents:
                if rng.random() < density:
                    edges.append((parent, child))
                    connected = True

            # garante conectividade: todo nó não-raiz precisa de ao menos 1 pai,
            # senão vira uma tarefa "solta" (isolada) e o DAG deixa de ser um
            # DAG único de fato.
            if not connected:
                forced_parent = rng.choice(levels[lvl_idx - 1])
                edges.append((forced_parent, child))

    return levels, edges, task_level


# ==============================
# 3. CUSTOS DE COMPUTAÇÃO (estilo Eq. 18 dos papers: wi ~ U(0, 2*wDAG))
# ==============================
def assign_computation_costs(n, flops_pool, mem_pool, rng):
    # ANTES: w_dag = rng.choice(flops_pool) — um único sorteio de uma
    # distribuição log-uniforme de ~4 décadas (1e9 a 1e13) decidia a escala
    # de tempo do workflow INTEIRO. Como a topologia é gerada antes do custo,
    # mudar fat/density/jump consome uma sequência diferente de chamadas de
    # rng, então a mesma seed acabava sorteando um wDAG ~1000x menor/maior
    # só por estar em outro ponto da sequência — colapsando o makespan de
    # ~10s para ~0.01s sem nenhuma mudança real de carga computacional.
    #
    # AGORA: wDAG é a MÉDIA do pool real (estável, representativa da carga
    # PolyBench de verdade), com um jitter multiplicativo pequeno (±30%,
    # não 4 décadas) para ainda variar levemente entre gerações.
    w_dag_base = statistics.mean(flops_pool)
    mem_dag_base = statistics.mean(mem_pool)
    w_dag = w_dag_base * rng.uniform(0.7, 1.3)
    mem_dag = mem_dag_base * rng.uniform(0.7, 1.3)

    flops = []
    memory = []
    for _ in range(n):
        wi = rng.uniform(0.1 * w_dag, 2.0 * w_dag)  # evita zero absoluto
        mi = rng.uniform(0.1 * mem_dag, 2.0 * mem_dag)
        flops.append(max(FLOPS_TARGET_MIN, wi))
        memory.append(max(MEM_TARGET_MIN, mi))
    return flops, memory


# ==============================
# 4. CUSTOS DE COMUNICAÇÃO CALIBRADOS PARA UM CCR ALVO
# ==============================
def assign_communication_bytes(edges, flops, ccr_target, rng):
    """
    CCR = soma(tempo de comunicação) / soma(tempo de computação).
    Tempo de computação de uma tarefa = flops / REFERENCE_GFLOPS (em Gf/s
    convertido de forma consistente; aqui tratamos como unidades relativas,
    o que basta pois só a RAZÃO importa).
    Distribui o "tempo de comunicação total" entre as arestas de forma
    heterogênea (pesos aleatórios) e converte para bytes via
    REFERENCE_BANDWIDTH_BPS.
    """
    if not edges:
        return {}

    total_compute_time = sum(flops) / REFERENCE_FLOPS_PER_SEC
    total_comm_time_target = ccr_target * total_compute_time

    if total_comm_time_target <= 0:
        return {e: 0 for e in edges}

    raw_weights = [rng.uniform(0.3, 1.7) for _ in edges]
    weight_sum = sum(raw_weights)

    comm_bytes = {}
    for edge, w in zip(edges, raw_weights):
        edge_time = total_comm_time_target * (w / weight_sum)
        size_bytes = edge_time * REFERENCE_BANDWIDTH_BPS
        comm_bytes[edge] = max(FILE_TARGET_MIN, min(FILE_TARGET_MAX, size_bytes))
    return comm_bytes


# ==============================
# 5. MONTAGEM DO JSON WRENCH (schema idêntico à versão anterior)
# ==============================
def estimate_disk_saturation_risk(flops, comm_bytes, platform_size):
    """
    O disco (DGX_System/nvme_storage) é um recurso ÚNICO e serializado — ao
    contrário da computação, que se divide entre `platform_size` GPUs, TODA
    comunicação do workflow converge para ele. Por isso o "CCR seguro" aqui
    não é o mesmo dos papers (que assumem rede fully-connected sem
    contenção): o tempo de disco não se divide por P, mas o de computação
    sim. Estimamos o tempo de parede esperado de cada lado e alertamos se o
    disco tende a dominar o makespan (o que transformaria o experimento em
    um benchmark de I/O, não de escalonamento de GPU).
    """
    total_compute_time = sum(flops) / REFERENCE_FLOPS_PER_SEC
    total_disk_time = sum(comm_bytes.values()) / REFERENCE_BANDWIDTH_BPS if comm_bytes else 0.0

    expected_compute_wall = total_compute_time / max(1, platform_size)
    expected_disk_wall = total_disk_time  # não paraleliza

    if expected_compute_wall <= 0:
        return None

    ratio = expected_disk_wall / (expected_compute_wall + expected_disk_wall)
    return ratio, expected_compute_wall, expected_disk_wall


def build_wrench_workflow(n, levels, edges, flops, memory, comm_bytes, rng,
                          target_tasks, platform_size, seed, ccr, fat, density,
                          regularity, jump):
    """
    Modelo PolyBench-fiel (Forma B): cada tarefa de computacao e UM unico job
    que le suas entradas (as saidas de seus predecessores) e escreve sua
    saida. Nao ha jobs de I/O separados por aresta — o I/O e do KERNEL, nao
    da comunicacao-via-disco. Isso reduz o numero de jobs de ~n*16 para n.

    Estrutura por tarefa (via "uses", formato que o Controller ja le):
      - 1 arquivo de saida  file_out_{tid}     (type="output")
      - k arquivos de entrada = saidas dos predecessores (type="input")

    O tamanho do arquivo de saida de cada tarefa e dimensionado a partir do
    CCR: a soma dos bytes de comunicacao das arestas que SAEM de uma tarefa
    vira o tamanho do arquivo que ela produz (todo consumidor le esse mesmo
    arquivo). Assim o CCR-alvo e preservado com um arquivo por tarefa.
    """
    jobs = []
    files = []
    dependencies = []

    # 1) Tamanho do arquivo de saida de cada tarefa: agrega os bytes das
    #    arestas que partem dela (a saida e lida por todos os filhos).
    out_bytes = {tid: 0.0 for tid in range(n)}
    for (parent, child) in edges:
        out_bytes[parent] += comm_bytes.get((parent, child), 0.0)
    # tarefas sem sucessor (exit) ainda escrevem um resultado minimo
    for tid in range(n):
        if out_bytes[tid] <= 0:
            out_bytes[tid] = FILE_TARGET_MIN

    # 2) Mapear predecessores (para declarar os inputs de cada tarefa)
    preds = {tid: [] for tid in range(n)}
    for (parent, child) in edges:
        preds[child].append(parent)

    # 3) Criar um arquivo de saida por tarefa
    for tid in range(n):
        fid = f"file_out_{tid}"
        files.append({"id": fid, "size": int(out_bytes[tid])})

    # 4) Criar os jobs de computacao com "uses" (output proprio + inputs dos pais)
    for tid in range(n):
        uses = [{"id": f"file_out_{tid}", "type": "output"}]
        for p in preds[tid]:
            uses.append({"id": f"file_out_{p}", "type": "input"})
        jobs.append({
            "id": f"task_{tid}",
            "flops": flops[tid],
            "memory": memory[tid],
            "uses": uses,
        })

    # 5) Dependencias = arestas do DAG (sem jobs intermediarios)
    for (parent, child) in edges:
        dependencies.append({
            "parents": [f"task_{parent}"],
            "children": [f"task_{child}"],
        })

    return {
        "name": f"wf_{target_tasks}_tasks_{platform_size}_cores",
        "seed": seed,
        "platform_size": platform_size,
        "generator": "layered_daggen",
        "params": {"fat": fat, "density": density, "regularity": regularity, "jump": jump, "ccr": ccr},
        "target_tasks": target_tasks,
        "jobs": jobs,
        "files": files,
        "dependencies": dependencies,
        "compute_jobs_count": n,
        "io_jobs_count": 0,
        "total_jobs_count": len(jobs),
        "io_model": "kernel_uses",
    }


# ==============================
# 6. VALIDAÇÃO DO DAG (idêntica à versão anterior)
# ==============================
def validate_workflow(wf, verbose=False):
    warnings = []
    jobs = wf["jobs"]
    files = wf.get("files", [])

    job_ids = [j["id"] for j in jobs]
    file_ids = {f["id"] for f in files}
    job_id_set = set(job_ids)

    dup_jobs = [jid for jid, cnt in collections.Counter(job_ids).items() if cnt > 1]
    if dup_jobs:
        warnings.append(f"IDs de job duplicados: {dup_jobs[:5]}")

    for job in jobs:
        for field in ("file_reads", "file_writes"):
            for item in job.get(field, []):
                if item["id"] not in file_ids:
                    warnings.append(f"Referência de arquivo inexistente: {item['id']}")

    in_degree = collections.defaultdict(int)
    adj = collections.defaultdict(list)
    for dep in wf.get("dependencies", []):
        for parent in dep["parents"]:
            for child in dep["children"]:
                if parent in job_id_set and child in job_id_set:
                    adj[parent].append(child)
                    in_degree[child] += 1
    queue = collections.deque(jid for jid in job_ids if in_degree[jid] == 0)
    visited = 0
    while queue:
        node = queue.popleft()
        visited += 1
        for neighbor in adj[node]:
            in_degree[neighbor] -= 1
            if in_degree[neighbor] == 0:
                queue.append(neighbor)
    has_cycle = visited != len(job_ids)
    if has_cycle:
        cycle_count = len(job_ids) - visited
        warnings.append(f"CICLO DETECTADO: {cycle_count} job(s) em dependência circular")

    flops = [j["flops"] for j in jobs if "flops" in j]
    cv = 0
    if flops and len(flops) > 1:
        cv = statistics.stdev(flops) / statistics.mean(flops) * 100
        if cv > 300:
            warnings.append(f"CV de FLOPs muito alto: {cv:.0f}%")

    if verbose:
        compute = [j for j in jobs if "flops" in j]
        io_only = [j for j in jobs if "flops" not in j]
        print(f"  Jobs compute : {len(compute)}")
        print(f"  Jobs I/O     : {len(io_only)}")
        if flops:
            print(f"  FLOPs CV     : {cv:.1f}%")
            print(f"  FLOPs range  : {min(flops):.2e} – {max(flops):.2e}")

    return warnings, has_cycle


# ==============================
# 7. GERAÇÃO PRINCIPAL
# ==============================
def generate_large_workflow(
    folder,
    target_tasks=1024,
    platform_size=32,
    fat=0.5,
    density=0.5,
    regularity=0.5,
    jump=1,
    ccr=1.0,
    seed=42,
    verbose=False,
    strict=True,
):
    rng = random.Random(seed)

    flops_pool, mem_pool = load_polybench_cost_pool(folder)

    levels, edges, task_level = generate_layered_topology(
        n=target_tasks, fat=fat, density=density, regularity=regularity, jump=jump, rng=rng
    )
    flops, memory = assign_computation_costs(target_tasks, flops_pool, mem_pool, rng)
    comm_bytes = assign_communication_bytes(edges, flops, ccr, rng)

    risk = estimate_disk_saturation_risk(flops, comm_bytes, platform_size)
    if risk is not None:
        ratio, compute_wall, disk_wall = risk
        if verbose or ratio > 0.4:
            print(f"[AVISO DE CALIBRAÇÃO] Fração estimada do makespan dominada pelo disco "
                  f"único: {ratio*100:.0f}% (compute≈{compute_wall:.2f}s paralelizado em "
                  f"{platform_size} GPUs, disco≈{disk_wall:.2f}s SEM paralelizar).")
            if ratio > 0.4:
                safe_ccr = 0.25 / platform_size
                print(f"  ⚠ Com --ccr {ccr} e {len(edges)} arestas, o disco tende a dominar o "
                      f"makespan — os 5 schedulers vão convergir para resultados parecidos "
                      f"porque a decisão de GPU deixa de importar. Para um experimento de "
                      f"ESCALONAMENTO DE GPU, use --ccr bem abaixo de 1/{platform_size} "
                      f"(ccr=1/P só empata disco com compute, ~50/50) — tente "
                      f"--ccr {safe_ccr:.4f} para o disco ficar em ~20% do makespan, ou "
                      f"reduza --density/--jump para gerar menos arestas. Se a intenção É "
                      f"estudar gargalo de I/O, ignore este aviso.")

    wf = build_wrench_workflow(
        n=target_tasks, levels=levels, edges=edges, flops=flops, memory=memory,
        comm_bytes=comm_bytes, rng=rng, target_tasks=target_tasks,
        platform_size=platform_size, seed=seed, ccr=ccr, fat=fat, density=density,
        regularity=regularity, jump=jump,
    )

    issues, has_cycle = validate_workflow(wf, verbose=verbose)

    if verbose:
        print("===================================")
        print(f"Tasks alvo (compute) : {target_tasks}")
        print(f"Níveis               : {len(levels)}")
        print(f"Largura média/nível  : {statistics.mean(len(l) for l in levels):.1f}")
        print(f"Arestas              : {len(edges)}")
        print(f"Jobs I/O inseridos   : {wf['io_jobs_count']}")
        print(f"fat={fat} density={density} regularity={regularity} jump={jump} ccr={ccr}")
        print(f"Seed                 : {seed}")
        print(f"Plataforma           : {platform_size} GPUs")
        if issues:
            print(f"AVISOS ({len(issues)}):")
            for w in issues:
                print(f"  ⚠ {w}")
        else:
            print("Validação: OK (sem ciclos)")
        print("===================================")

    if has_cycle and strict:
        raise RuntimeError(
            "Geração ABORTADA: ciclo detectado no DAG. Um ciclo travaria a "
            "simulação WRENCH num loop infinito de espera por predecessores. "
            "Rode com --no-strict para ignorar (não recomendado) ou revise "
            "os parâmetros de topologia (jump/density muito agressivos podem "
            "criar ciclos se combinados incorretamente)."
        )

    return wf


# ==============================
# 8. GERAÇÃO EM LOTE
# ==============================
def generate_batch(
    folder, target_tasks, platform_size, fat, density, regularity, jump, ccr,
    n_trials=10, scenario_id=0, output_dir=".", verbose=False, strict=True,
):
    os.makedirs(output_dir, exist_ok=True)
    generated = []

    for trial_index in range(1, n_trials + 1):
        seed = (trial_index * 1000) + scenario_id  # fórmula do paper

        wf = generate_large_workflow(
            folder=folder, target_tasks=target_tasks, platform_size=platform_size,
            fat=fat, density=density, regularity=regularity, jump=jump, ccr=ccr,
            seed=seed, verbose=False, strict=strict,
        )
        fname = f"wf_{target_tasks}_tasks_{platform_size}_cores_trial{trial_index:02d}.json"
        fpath = os.path.join(output_dir, fname)
        with open(fpath, "w") as f:
            json.dump(wf, f, indent=2)
        generated.append(fpath)

        if verbose or trial_index % 5 == 0:
            print(f"  Trial {trial_index:02d}/{n_trials} | seed={seed} | "
                  f"jobs={wf['total_jobs_count']} (compute={wf['compute_jobs_count']}) → {fname}")

    return generated


# ==============================
# MAIN
# ==============================
if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Gerador de workflows sintéticos (topologia em níveis, estilo "
                    "daggen/Suter, com custo de tarefa realista via PolyBench)."
    )
    parser.add_argument("--folder", default=DEFAULT_BASE_FOLDER,
                        help="Pasta (recursiva) com blocos PolyBench, usada só para "
                             "amostrar custo computacional realista (wDAG)")
    parser.add_argument("--tasks", type=int, default=1024,
                        choices=[256, 512, 1024, 2048])
    parser.add_argument("--platform", type=int, default=32,
                        choices=[8, 16, 32, 64],
                        help="Nº de GPUs alvo — só usado para nome do arquivo de saída "
                             "(a heterogeneidade real vem do platform_Ngpu.xml)")

    parser.add_argument("--fat", type=float, default=0.5,
                        help="Largura média por nível = fat*sqrt(n). Papers testam [0.1, 0.4, 0.8]")
    parser.add_argument("--density", type=float, default=0.5,
                        help="Probabilidade de aresta por par (pai candidato, filho). Papers: [0.2, 0.8]")
    parser.add_argument("--regularity", type=float, default=0.5,
                        help="Uniformidade da largura entre níveis (1=uniforme). Papers: [0.2, 0.8]")
    parser.add_argument("--jump", type=int, default=1,
                        help="Distância máxima (em níveis) que uma aresta pode cruzar. Papers: [1, 2, 4]")
    parser.add_argument("--ccr", type=float, default=None,
                        help="Razão comunicação/computação alvo. Se omitido, usa 0.25/platform "
                             "(mantém o disco único em ~20%% do makespan). Papers usam [0.1..30], "
                             "mas isso assume rede sem contenção — aqui há 1 disco só; use valores "
                             "altos apenas para estudar gargalo de I/O de propósito.")

    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--no-strict", dest="strict", action="store_false",
                        help="Não abortar se detectar ciclo (NÃO recomendado)")

    parser.add_argument("--batch", action="store_true")
    parser.add_argument("--n-trials", type=int, default=10)
    parser.add_argument("--scenario-id", type=int, default=0)
    parser.add_argument("--output-dir", default=None)

    args = parser.parse_args()

    if args.ccr is None:
        args.ccr = 0.25 / args.platform
        print(f"[INFO] --ccr não especificado; usando {args.ccr:.4f} (0.25/{args.platform} GPUs) "
              f"para manter o disco único fora do caminho crítico.")

    default_dir = os.path.join(DEFAULT_WORKFLOWS_ROOT, f"{args.tasks}_tasks")
    output_dir = args.output_dir or (
        os.path.join(default_dir, "trials") if args.batch else default_dir
    )

    if args.batch:
        print(f"Gerando {args.n_trials} trials para {args.tasks} tasks / {args.platform} GPUs "
              f"(fat={args.fat}, density={args.density}, regularity={args.regularity}, "
              f"jump={args.jump}, ccr={args.ccr}, scenario_id={args.scenario_id})...")
        files = generate_batch(
            folder=args.folder, target_tasks=args.tasks, platform_size=args.platform,
            fat=args.fat, density=args.density, regularity=args.regularity, jump=args.jump,
            ccr=args.ccr, n_trials=args.n_trials, scenario_id=args.scenario_id,
            output_dir=output_dir, verbose=True, strict=args.strict,
        )
        print(f"\nConcluído. {len(files)} arquivos em: {output_dir}")
    else:
        workflow = generate_large_workflow(
            folder=args.folder, target_tasks=args.tasks, platform_size=args.platform,
            fat=args.fat, density=args.density, regularity=args.regularity, jump=args.jump,
            ccr=args.ccr, seed=args.seed, verbose=args.verbose, strict=args.strict,
        )
        os.makedirs(output_dir, exist_ok=True)
        output_name = os.path.join(output_dir, f"wf_{args.tasks}_tasks_{args.platform}_cores.json")
        with open(output_name, "w") as f:
            json.dump(workflow, f, indent=2)
        print(f"Arquivo gerado: {output_name}")