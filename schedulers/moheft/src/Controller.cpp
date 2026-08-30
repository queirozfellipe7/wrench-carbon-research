#include "Controller.h"
#include <fstream>
#include <limits>
#include <algorithm>
#include <functional>
#include <iomanip>
#include <simgrid/s4u/Host.hpp>
#include "json.hpp"

WRENCH_LOG_CATEGORY(controller, "Log category for Controller");

using json = nlohmann::json;

// Parâmetros MOHEFT
const int MOHEFT_K = 32;               // número máximo de soluções mantidas (típico: 5–10)
// CARBON_INTENSITY (450.0) removida — substituída por host_carbon_intensity,
// lida diretamente do XML da plataforma (ver Fase de leitura de hosts).

// Compara se solução A domina B (multiobjetivo: makespan + energia + carbono)
bool Controller::dominates(const Solution& a, const Solution& b) const {
    bool better_makespan = a.makespan <= b.makespan;
    bool better_energy   = a.energy_joules <= b.energy_joules;
    bool better_carbon   = a.carbon_gco2   <= b.carbon_gco2;

    bool strictly_better = (a.makespan < b.makespan) ||
                           (a.energy_joules < b.energy_joules) ||
                           (a.carbon_gco2 < b.carbon_gco2);

    return better_makespan && better_energy && better_carbon && strictly_better;
}

// Seleciona uma solução de compromisso da fronteira (pesos exemplificativos –
// podem ser ajustados conforme a prioridade da pesquisa).
//
// CORREÇÃO: a versão anterior somava os 3 objetivos em unidades BRUTAS
// (segundos + megajoules + quilogramas) antes de aplicar o peso. Como as
// magnitudes reais deste projeto são muito diferentes (makespan ~1-5,
// energia_MJ ~0.01-0.05, carbono_kg ~0.001-0.003), o termo de makespan
// dominava o score em ~99,6% dos casos testados — a escolha final ficava
// makespan-only na prática, apesar da fronteira mantida durante a busca
// (crowding distance) ser genuinamente diversa nos 3 objetivos. Agora cada
// objetivo é normalizado pelo próprio intervalo (min-max) DENTRO da
// fronteira antes do peso, tornando os 3 termos comparáveis em escala.
Controller::Solution Controller::select_best_solution(const std::vector<Solution>& pareto_front) const {
    if (pareto_front.empty()) return Solution{};
    if (pareto_front.size() == 1) return pareto_front[0];

    auto min_max = [&](auto getter) {
        double mn = getter(pareto_front[0]), mx = getter(pareto_front[0]);
        for (const auto& s : pareto_front) {
            mn = std::min(mn, getter(s));
            mx = std::max(mx, getter(s));
        }
        return std::make_pair(mn, mx);
    };
    auto mk = min_max([](const Solution& s) { return s.makespan; });
    auto en = min_max([](const Solution& s) { return s.energy_joules; });
    auto cb = min_max([](const Solution& s) { return s.carbon_gco2; });

    auto normalize = [](double v, double mn, double mx) {
        return (mx > mn) ? (v - mn) / (mx - mn) : 0.0;  // sem variação -> não penaliza nem favorece
    };

    Solution best = pareto_front[0];
    double best_score = std::numeric_limits<double>::max();

    for (const auto& s : pareto_front) {
        double score = 0.50 * normalize(s.makespan, mk.first, mk.second) +
                       0.30 * normalize(s.energy_joules, en.first, en.second) +
                       0.20 * normalize(s.carbon_gco2, cb.first, cb.second);
        if (score < best_score) {
            best_score = score;
            best = s;
        }
    }
    return best;
}

Controller::Controller(
    const std::shared_ptr<wrench::BareMetalComputeService> &bare_metal_compute_service,
    const std::shared_ptr<wrench::SimpleStorageService> &storage_service,
    const std::string &hostname,
    const std::string &json_file_path
) : wrench::ExecutionController(hostname, "controller"),
    bare_metal_compute_service(bare_metal_compute_service),
    storage_service(storage_service),
    json_file_path(json_file_path) {}

int Controller::main() {
    wrench::TerminalOutput::setThisProcessLoggingColor(wrench::TerminalOutput::COLOR_GREEN);
    WRENCH_INFO("Controller MOHEFT iniciando (K=%d)", MOHEFT_K);

    double start_time = this->getSimulation()->getCurrentSimulatedDate();

    // ---------- 1. Leitura do JSON ----------
    std::ifstream f(json_file_path);
    if (!f.is_open()) {
        std::cerr << "\n[ERRO FATAL] Não foi possível abrir o arquivo de workflow: "
                  << json_file_path << "\n"
                  << "Verifique o caminho passado como segundo argumento do simulador.\n"
                  << std::endl;
        throw std::runtime_error("Workflow JSON não encontrado: " + json_file_path);
    }
    json data = json::parse(f);

    if (data["jobs"].empty()) {
        std::cerr << "\n[ERRO FATAL] O workflow foi lido, mas contém ZERO jobs: "
                  << json_file_path << "\n" << std::endl;
        throw std::runtime_error("Workflow sem jobs: " + json_file_path);
    }

    // ---------- 2. Criação dos arquivos ----------
    std::map<std::string, std::shared_ptr<wrench::DataFile>> files;
    // CORREÇÃO (infraestrutura ausente): o MOHEFT nunca capturava o tamanho
    // dos arquivos nem qual tarefa lê/escreve qual arquivo — sem isso, era
    // impossível calcular custo de comunicação de I/O (mesmo padrão já
    // presente no HEFT/PEFT/IPEFT desde o início, nunca replicado aqui).
    std::map<std::string, double> file_sizes;
    for (auto const &file : data["files"]) {
        std::string file_id = file["id"];
        sg_size_t file_size = file["size"].get<sg_size_t>();
        file_sizes[file_id] = static_cast<double>(file_size);
        files[file_id] = wrench::Simulation::addFile(file_id, file_size);
        wrench::StorageService::createFileAtLocation(
            wrench::FileLocation::LOCATION(this->storage_service, files[file_id])
        );
    }

    auto job_manager = this->createJobManager();

    // ---------- 3. Criação dos jobs e Estruturas do Grafo ----------
    std::map<std::string, std::shared_ptr<wrench::CompoundJob>> jobs;
    std::map<std::string, double> comp_cost;
    std::map<std::string, std::vector<std::string>> successors;
    std::map<std::string, std::vector<std::string>> predecessors;
    // CORREÇÃO (mesma infraestrutura ausente descrita acima): bytes por
    // tarefa de I/O e arquivos de entrada/saída, necessários pra resolver
    // o predecessor computacional real através da cadeia de I/O (ver
    // resolve_compute_predecessors mais abaixo).
    std::map<std::string, double> io_bytes;
    std::map<std::string, std::vector<std::string>> task_output_files;
    std::map<std::string, std::vector<std::string>> task_input_files;
    for (auto const &task : data["jobs"]) {
        std::string id = task["id"];
        auto job = job_manager->createCompoundJob(id);
        jobs[id] = job;
        if (task.contains("file_reads") && !task["file_reads"].empty()) {
            std::string file_id = task["file_reads"][0]["id"];
            job->addFileReadAction(id, wrench::FileLocation::LOCATION(this->storage_service, files[file_id]));
            comp_cost[id] = 0.0;
            io_bytes[id] = file_sizes.count(file_id) ? file_sizes.at(file_id) : 0.0;
            task_input_files[id].push_back(file_id);
        } else if (task.contains("file_writes") && !task["file_writes"].empty()) {
            std::string file_id = task["file_writes"][0]["id"];
            job->addFileWriteAction(id, wrench::FileLocation::LOCATION(this->storage_service, files[file_id]));
            comp_cost[id] = 0.0;
            io_bytes[id] = file_sizes.count(file_id) ? file_sizes.at(file_id) : 0.0;
            task_output_files[id].push_back(file_id);
        } else {
            int cores = task.contains("cores") ? (int)task["cores"] : 1;
            double flops = task["flops"].get<double>();
            double memory = task.contains("memory") ? task["memory"].get<double>() : 0.0;
            job->addComputeAction(id, flops, memory, 1, cores, wrench::ParallelModel::AMDAHL(0.9));
            comp_cost[id] = flops;
        }
    }
    // ---------- 4. Dependências ----------
    if (data.contains("dependencies")) {
        for (auto const &dep : data["dependencies"]) {
            for (auto const &p : dep["parents"]) {
                for (auto const &c : dep["children"]) {
                    std::string parent = p;
                    std::string child = c;
                    predecessors[child].push_back(parent);
                    successors[parent].push_back(child);
                    jobs[child]->addParentJob(jobs[parent]);
                }
            }
        }
    }
    // ---------- Hosts e propriedades energéticas (adicionado) ----------
    auto hosts_capabilities = bare_metal_compute_service->getCoreFlopRate();
    std::vector<std::string> host_list;
    for (auto const &h : hosts_capabilities) host_list.push_back(h.first);

    // Potência (idle/max) lida DIRETAMENTE das propriedades do platform.xml
    // (wattage_per_state="idle:max"), em vez de assumida por nome do host.
    // O hardcode anterior só reconhecia GPU1-4 como "alta potência" e tratava
    // TODAS as outras como baixa potência (250W) — o que está errado a
    // partir de 16 GPUs, já que o padrão rápida/lenta do platform.xml se
    // repete a cada bloco de 8 (GPU9-12, GPU17-20, GPU25-28 também são
    // rápidas/alta potência, mas o código antigo não sabia disso). Ler a
    // propriedade real torna o modelo válido para qualquer número de GPUs.
    std::map<std::string, double> host_pmax, host_pidle;
    for (const auto& h : host_list) {
        double pidle = 50.0, pmax = 250.0;  // fallback conservador, só se a prop faltar
        auto sg_host = simgrid::s4u::Host::by_name_or_null(h);
        const char* wattage_str = sg_host ? sg_host->get_property("wattage_per_state") : nullptr;
        if (wattage_str) {
            std::string s(wattage_str);
            size_t colon = s.find(':');
            if (colon != std::string::npos) {
                try {
                    pidle = std::stod(s.substr(0, colon));
                    pmax  = std::stod(s.substr(colon + 1));
                } catch (const std::exception&) {
                    WRENCH_WARN("wattage_per_state inválido no host %s, usando fallback.", h.c_str());
                }
            }
        } else {
            WRENCH_WARN("Host %s sem propriedade wattage_per_state; usando fallback.", h.c_str());
        }
        host_pidle[h] = pidle;
        host_pmax[h]  = pmax;
    }

    // Intensidade de carbono lida DIRETAMENTE das propriedades do
    // platform.xml (carbon_intensity), mesmo padrão acima usado para
    // wattage_per_state. Substitui a constante global CARBON_INTENSITY
    // (450.0), que divergia do valor real declarado nos arquivos de
    // plataforma deste projeto (473) — o mesmo tipo de problema já
    // corrigido para a potência (assumir um único valor hardcoded em vez
    // de ler a propriedade real do host).
    std::map<std::string, double> host_carbon_intensity;
    for (const auto& h : host_list) {
        double intensity = 473.0;  // fallback conservador, só se a prop faltar
        auto sg_host_ci = simgrid::s4u::Host::by_name_or_null(h);
        const char* intensity_str = sg_host_ci ? sg_host_ci->get_property("carbon_intensity") : nullptr;
        if (intensity_str) {
            try {
                intensity = std::stod(intensity_str);
            } catch (const std::exception&) {
                WRENCH_WARN("carbon_intensity inválido no host %s, usando fallback.", h.c_str());
            }
        } else {
            WRENCH_WARN("Host %s sem propriedade carbon_intensity; usando fallback.", h.c_str());
        }
        host_carbon_intensity[h] = intensity;
    }

    // CORREÇÃO (infraestrutura ausente, mesmo padrão do HEFT/PEFT/IPEFT):
    // banda de I/O por host, necessária pra calcular o atraso de
    // comunicação real entre tarefas de computação que trocam dados via
    // arquivo (ver resolve_compute_predecessors mais abaixo).
    std::map<std::string, double> host_io_bandwidth;
    for (const auto& h : host_list) {
        double bw = 1e9;  // fallback conservador: 1 GB/s
        auto sg_host_bw = simgrid::s4u::Host::by_name_or_null(h);
        const char* bw_str = sg_host_bw ? sg_host_bw->get_property("io_bandwidth_Bps") : nullptr;
        if (bw_str) {
            try { bw = std::stod(bw_str); } catch (const std::exception&) {
                WRENCH_WARN("io_bandwidth_Bps inválido no host %s, usando fallback.", h.c_str());
            }
        } else {
            WRENCH_WARN("Host %s sem propriedade io_bandwidth_Bps; usando fallback.", h.c_str());
        }
        host_io_bandwidth[h] = bw;
    }

    // ---------- 5. MOHEFT Fase 1: Prioritização ----------
    double avg_flop_rate = 0.0;
    for (auto const &h : hosts_capabilities) avg_flop_rate += h.second;
    if (!hosts_capabilities.empty()) avg_flop_rate /= hosts_capabilities.size();
    else avg_flop_rate = 1.0;

    // CORREÇÃO (mesmo desempate já validado no HEFT/PEFT/IPEFT): tarefas de
    // I/O têm peso w=0 na fórmula de rank_u, herdando o rank de toda a
    // cadeia à frente — isso empata ~2/3 das arestas do grafo. std::sort
    // não é estável, então sem um desempate explícito a ordem relativa
    // entre uma tarefa e sua sucessora pode sair invertida.
    std::map<std::string, int> topo_index;
    {
        std::map<std::string, int> indeg;
        for (auto const &j : jobs) indeg[j.first] = 0;
        for (auto const &pr : predecessors) indeg[pr.first] = (int)pr.second.size();
        std::vector<std::string> queue;
        for (auto const &j : jobs) if (indeg[j.first] == 0) queue.push_back(j.first);
        std::sort(queue.begin(), queue.end());
        size_t qi = 0;
        int idx = 0;
        while (qi < queue.size()) {
            std::string u = queue[qi++];
            topo_index[u] = idx++;
            for (auto const &v : successors[u]) {
                if (--indeg[v] == 0) queue.push_back(v);
            }
        }
    }

    std::map<std::string, double> rank_u;
    std::function<double(const std::string&)> compute_rank;
    compute_rank = [&](const std::string &id) -> double {
        if (rank_u.count(id)) return rank_u[id];
        double w = (comp_cost[id] > 0) ? comp_cost[id] / avg_flop_rate : 0.0;
        double max_succ = 0.0;
        for (auto const &s : successors[id]) max_succ = std::max(max_succ, compute_rank(s));
        rank_u[id] = w + max_succ;
        return rank_u[id];
    };
    for (auto const &j : jobs) compute_rank(j.first);

    std::vector<std::string> task_order;
    for (auto const &j : jobs) task_order.push_back(j.first);
    std::sort(task_order.begin(), task_order.end(), [&](const std::string &a, const std::string &b) {
        if (std::abs(rank_u[a] - rank_u[b]) > 1e-9) return rank_u[a] > rank_u[b];
        return topo_index[a] < topo_index[b];
    });

    // CORREÇÃO CRÍTICA: o predecessor DIRETO de uma tarefa de computação
    // neste projeto é sempre um nó de I/O (task_A -> write_A_B -> read_A_B
    // -> task_B), nunca outra tarefa de computação diretamente. Como a
    // Fase 2 abaixo pula tarefas de I/O (nunca entram em curr.finish_time),
    // procurar "predecessors[task]" direto em curr.finish_time falhava
    // SEMPRE — confirmado: 100% dos predecessores de tarefas de computação
    // no workflow real são jobs de I/O (1790/1790 arestas testadas).
    // ready_time saía 0.0 silenciosamente pra praticamente toda tarefa do
    // grafo, como se todo dado já estivesse disponível desde o início.
    // Esta função atravessa a cadeia de I/O pra trás até achar o(s)
    // predecessor(es) computacional(is) de verdade, acumulando o tamanho
    // do arquivo transferido (memoizada — cada tarefa só é resolvida uma
    // vez, independente de quantas vezes é consultada).
    std::map<std::string, std::vector<std::pair<std::string, double>>> logical_pred_cache;
    std::function<std::vector<std::pair<std::string, double>>(const std::string&)> resolve_compute_predecessors;
    resolve_compute_predecessors = [&](const std::string &id) -> std::vector<std::pair<std::string, double>> {
        if (logical_pred_cache.count(id)) return logical_pred_cache.at(id);
        std::vector<std::pair<std::string, double>> result;
        for (const auto &p : predecessors[id]) {
            if (comp_cost.count(p) && comp_cost.at(p) > 0) {
                result.push_back({p, 0.0});
            } else {
                double bytes_here = io_bytes.count(p) ? io_bytes.at(p) : 0.0;
                for (auto &dp : resolve_compute_predecessors(p)) {
                    result.push_back({dp.first, std::max(dp.second, bytes_here)});
                }
            }
        }
        logical_pred_cache[id] = result;
        return result;
    };

    // ---------- 6. MOHEFT Fase 2: Seleção de Recursos ----------
    std::vector<Solution> front = {Solution{}};
    for (const auto& task : task_order) {
        if (comp_cost[task] == 0.0) continue;  // Pular tarefas I/O (não computacionais)

        auto logical_preds = resolve_compute_predecessors(task);
        std::vector<Solution> candidates;

        for (const auto& curr : front) {
            for (const auto& hostname : host_list) {
                double flop_rate = hosts_capabilities[hostname];
                double execution_time = (comp_cost[task] > 0) ? comp_cost[task] / flop_rate : 0.001;

                // ready_time real: usa os predecessores COMPUTACIONAIS
                // resolvidos (não predecessors[task] direto, que são nós
                // de I/O e nunca aparecem em curr.finish_time), somando
                // custo de comunicação se o predecessor rodou num host
                // diferente do candidato atual.
                double ready_time = 0.0;
                for (const auto& pb : logical_preds) {
                    const std::string& p = pb.first;
                    double bytes = pb.second;
                    auto it = curr.finish_time.find(p);
                    double base = (it != curr.finish_time.end()) ? it->second : 0.0;
                    std::string p_host = curr.assignment.count(p) ? curr.assignment.at(p) : hostname;
                    double comm = (p_host != hostname && bytes > 0)
                        ? bytes / std::min(host_io_bandwidth.at(p_host), host_io_bandwidth.at(hostname))
                        : 0.0;
                    ready_time = std::max(ready_time, base + comm);
                }

                double avail = 0.0;
                auto it_host = curr.host_avail.find(hostname);
                if (it_host != curr.host_avail.end()) {
                    avail = it_host->second;
                }

                double est = std::max(avail, ready_time);
                double eft = est + execution_time;

                Solution news = curr;
                news.assignment[task] = hostname;
                news.finish_time[task] = eft;
                news.host_avail[hostname] = eft;
                news.makespan = std::max(news.makespan, eft);
                news.active_time[hostname] += execution_time;
                // CORRECAO ENERGIA (modelo fisico, coerente com o plugin do SimGrid):
                //   E = SOMA_h [ pidle_h * makespan + (pmax_h - pidle_h) * ativo_h ]
                // Recalculada AQUI (nao so no fim) porque dominates() e o crowding
                // usam energy_joules/carbon_gco2 DURANTE a busca. O modelo antigo
                // (pmax*ativo apenas) ignorava o idle e premiava espalhar tarefas
                // em GPUs lentas -> solucao dominada em makespan E energia.
                double E = 0.0, C = 0.0;
                for (auto const &hh : host_list) {
                    double at = news.active_time.count(hh) ? news.active_time.at(hh) : 0.0;
                    double e_h = host_pidle[hh] * news.makespan
                               + (host_pmax[hh] - host_pidle[hh]) * at;
                    E += e_h;
                    C += (e_h / 3.6e6) * host_carbon_intensity[hh];
                }
                news.energy_joules = E;
                news.carbon_gco2   = C;

                candidates.push_back(news);
            }
        }

        // Filtrar não-dominadas
        std::vector<Solution> new_front;
        for (auto& cand : candidates) {
            bool dominated = false;
            for (auto it = new_front.begin(); it != new_front.end(); ) {
                if (dominates(*it, cand)) {
                    dominated = true;
                    break;
                }
                if (dominates(cand, *it)) {
                    it = new_front.erase(it);
                } else {
                    ++it;
                }
            }
            if (!dominated) {
                new_front.push_back(cand);
            }
        }

        // Limitar a K: poda por distância de aglomeração (crowding
        // distance), preservando diversidade nos 3 objetivos (makespan,
        // energia, carbono) em vez de manter só as soluções de menor
        // makespan. A poda anterior (sort só por makespan) colapsava
        // progressivamente a fronteira num ranking de makespan disfarçado
        // de multiobjetivo, descartando sistematicamente soluções
        // competitivas em energia/carbono mas medianas em makespan — um
        // viés presente em TODA execução do MOHEFT já realizada com essa
        // versão do código, não um desvio pontual.
        if (new_front.size() > static_cast<size_t>(MOHEFT_K)) {
            size_t n = new_front.size();
            for (auto &s : new_front) s.crowding_distance = 0.0;

            auto assign_crowding = [&](auto getter) {
                std::vector<size_t> idx(n);
                for (size_t i = 0; i < n; ++i) idx[i] = i;
                std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
                    return getter(new_front[a]) < getter(new_front[b]);
                });
                double range = getter(new_front[idx[n - 1]]) - getter(new_front[idx[0]]);
                new_front[idx[0]].crowding_distance = std::numeric_limits<double>::max();
                new_front[idx[n - 1]].crowding_distance = std::numeric_limits<double>::max();
                if (range <= 0) return;
                for (size_t i = 1; i + 1 < n; ++i) {
                    new_front[idx[i]].crowding_distance +=
                        (getter(new_front[idx[i + 1]]) - getter(new_front[idx[i - 1]])) / range;
                }
            };

            assign_crowding([](const Solution& s) { return s.makespan; });
            assign_crowding([](const Solution& s) { return s.energy_joules; });
            assign_crowding([](const Solution& s) { return s.carbon_gco2; });

            std::sort(new_front.begin(), new_front.end(),
                      [](const Solution& a, const Solution& b) { return a.crowding_distance > b.crowding_distance; });
            new_front.resize(MOHEFT_K);
        }

        front = std::move(new_front);
    }

    // ---------- 7. Selecionar melhor solução da fronteira ----------
    Solution best = select_best_solution(front);

    // CORREÇÃO DE BUG: a Fase 2 do MOHEFT pula tarefas de I/O de propósito
    // ("Pular tarefas I/O (não computacionais)"), então elas NUNCA recebem
    // uma entrada em best.assignment. Sem esta correção, best.assignment[id]
    // criaria uma entrada com host = "" (string vazia) para essas tarefas,
    // e submitJob() falharia/travaria ao tentar rodar num host inexistente.
    // Atribuímos essas tarefas ao host de armazenamento (onde os arquivos
    // já residem), que é o destino natural para operações de I/O.
    for (auto const &id : task_order) {
        if (best.assignment.find(id) == best.assignment.end()) {
            best.assignment[id] = "DGX_System";
        }
    }

    // ---------- 8. Loop de Execução (adaptado para MOHEFT) ----------
    std::set<std::string> completed;
    std::set<std::string> submitted;
    std::map<std::string, std::string> actual_execution_host = best.assignment;

    // Medição REAL de carga por host (compute + I/O), via merge de
    // intervalos [submissão, conclusão]. Substitui a estimativa teórica
    // "comp_cost/flop_rate" que era usada antes e que ignorava por
    // completo os jobs de I/O (mesma limitação corrigida no HEFT).
    std::map<std::string, double> task_submit_time;
    std::map<std::string, std::vector<std::pair<double, double>>> host_intervals;
    std::map<std::string, double> task_actual_duration; // info bruta p/ CSV
    // Valor REAL de conclusão de cada tarefa, medido no evento de término.
    // Separado de best.finish_time (que vem da BUSCA multiobjetivo teórica,
    // Fase 2, e nunca tem entrada para tarefas de I/O, já que elas são
    // puladas de propósito ali — comp_cost[task]==0.0 -> continue). Sem
    // este mapa, o CSV usaria best.finish_time.find(id), que cai no
    // fallback 0.0 para toda tarefa de I/O (não uma previsão desatualizada,
    // um zero absoluto) e a previsão teórica teórica para as demais.
    std::map<std::string, double> task_real_finish_time;

    while (completed.size() < jobs.size()) {
        for (auto const &id : task_order) {
            if (submitted.count(id)) continue;
            bool ready = true;
            for (auto const &p : predecessors[id]) {
                if (completed.find(p) == completed.end()) {
                    ready = false;
                    break;
                }
            }
            if (ready) {
                std::map<std::string, std::string> service_args;
                service_args[id] = best.assignment[id];
                task_submit_time[id] = this->getSimulation()->getCurrentSimulatedDate();
                job_manager->submitJob(jobs[id], bare_metal_compute_service, service_args);
                submitted.insert(id);
            }
        }
        if (completed.size() < jobs.size()) {
            auto event = this->waitForNextEvent();
            if (auto ev = std::dynamic_pointer_cast<wrench::CompoundJobCompletedEvent>(event)) {
                std::string finished_id = ev->job->getName();
                completed.insert(finished_id);

                double now = this->getSimulation()->getCurrentSimulatedDate();
                double submit_t = task_submit_time[finished_id];
                task_actual_duration[finished_id] = now - submit_t;
                task_real_finish_time[finished_id] = now;
                host_intervals[actual_execution_host[finished_id]].push_back({submit_t, now});

            } else if (auto ev_fail = std::dynamic_pointer_cast<wrench::CompoundJobFailedEvent>(event)) {
                WRENCH_WARN("Job %s failed!", ev_fail->job->getName().c_str());
                return 1;
            }
        }
    }
    double end_time = this->getSimulation()->getCurrentSimulatedDate();
    // CAMINHO 2 (MOHEFT): makespan = makespan da solucao escolhida na fronteira
    // de Pareto (best.makespan, ja analitico e com comunicacao heterogenea).
    // Garante que makespan, energia e carbono venham da MESMA Solution 'best'
    // — coerencia multiobjetivo. Substitui end_time-start_time (fisico/disco).
    this->makespan = best.makespan;
    (void) end_time;

    // Merge de intervalos por host — evita dupla contagem quando várias
    // tarefas ficam enfileiradas no mesmo host (mesma correção do HEFT).
    for (auto &entry : host_intervals) {
        auto &intervals = entry.second;
        if (intervals.empty()) continue;
        std::sort(intervals.begin(), intervals.end());

        double merged_total = 0.0;
        double cur_start = intervals[0].first;
        double cur_end   = intervals[0].second;
        for (size_t i = 1; i < intervals.size(); ++i) {
            double s = intervals[i].first, e = intervals[i].second;
            if (s <= cur_end) {
                cur_end = std::max(cur_end, e);
            } else {
                merged_total += (cur_end - cur_start);
                cur_start = s;
                cur_end   = e;
            }
        }
        merged_total += (cur_end - cur_start);
        host_total_load[entry.first] = merged_total;
    }
    for (const auto& h : host_list) {
        if (host_total_load.find(h) == host_total_load.end()) host_total_load[h] = 0.0;
    }

    // ---------- 9. Processamento de Dados (Corrigido) ----------
    // 9.1. Salva os dados detalhados apenas no CSV com nome dinâmico
    std::string csv_filename = "resultados_" + this->getWorkflowName() + "_MOHEFT.csv";
    std::ofstream csv_file(csv_filename);
   
    if (csv_file.is_open()) {
        csv_file << "task_id,host,finish_time,load_seconds\n";
        for (auto const &id : task_order) {
            auto it_finish = task_real_finish_time.find(id);
            double finish = (it_finish != task_real_finish_time.end()) ? it_finish->second : 0.0;
            csv_file << id << ","
                     << actual_execution_host[id] << ","
                     << std::fixed << std::setprecision(4) << finish << ","
                     << task_actual_duration[id] << "\n";
        }
        csv_file.close();
        WRENCH_INFO("Log detalhado salvo em %s", csv_filename.c_str());
    }
    // 9.2. Exibe no terminal APENAS o resumo de uso e Makespan
    std::cout << "\n============================================================" << std::endl;
    std::cout << " RESUMO DE UTILIZAÇÃO DA PLATAFORMA" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << std::left << std::setw(20) << "Host" << " | " << "Tempo Total de Uso (s)" << std::endl;
    std::cout << "------------------------------------------------------------" << std::endl;
    // host_total_load agora é membro da classe, já preenchido pelo merge
    // de intervalos reais (compute + I/O) feito logo após o loop de execução.

    for (auto const &h : hosts_capabilities) {
        std::cout << std::left << std::setw(20) << h.first << " | "
                  << std::fixed << std::setprecision(4) << host_total_load[h.first] << " s" << std::endl;
    }
   
    std::cout << "------------------------------------------------------------" << std::endl;
    std::cout << "Makespan Total: " << std::fixed << std::setprecision(4) << this->makespan << " s" << std::endl;
    std::cout << "============================================================" << std::endl;

    return 0;
}

std::string Controller::getWorkflowName(){
    // Pega o nome do arquivo a partir do caminho completo
    size_t last_slash = json_file_path.find_last_of("/\\");
    std::string name = (last_slash == std::string::npos) ? json_file_path : json_file_path.substr(last_slash + 1);
   
    // Remove a extensão .json
    size_t last_dot = name.find_last_of(".");
    if (last_dot != std::string::npos) name = name.substr(0, last_dot);
   
    return name;
    }