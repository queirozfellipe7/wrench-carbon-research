/**
 * Copyright (c) 2017-2021. The WRENCH Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

/**
 ** An execution controller to execute a workflow
 **/

#include "Controller.h"
#include <fstream>
#include <limits>
#include <algorithm>
#include <functional>
#include <iomanip>
#include <cmath>
#include "json.hpp"

WRENCH_LOG_CATEGORY(controller, "Log category for Controller");

using json = nlohmann::json;

Controller::Controller(
    const std::shared_ptr<wrench::BareMetalComputeService> &bare_metal_compute_service,
    const std::shared_ptr<wrench::SimpleStorageService> &storage_service,
    const std::string &hostname,
    const std::string &json_file_path
) : wrench::ExecutionController(hostname, "controller"),
    bare_metal_compute_service(bare_metal_compute_service),
    storage_service(storage_service),
    json_file_path(json_file_path) {}

// Função para computar OCT (mantida para PEFT)
std::map<std::pair<std::string, std::string>, double> Controller::compute_oct(
    const std::map<std::string, double>& comp_cost,
    const std::map<std::string, std::vector<std::string>>& successors,
    const std::vector<std::string>& host_list,
    const std::map<std::string, double>& hosts_capabilities,
    const std::vector<std::string>& task_order_reverse,  // ordem topológica reversa
    const std::map<std::string, double>& io_bytes,
    const std::map<std::string, double>& host_io_bandwidth,
    const std::map<std::pair<std::string, std::string>, double>& edge_data_bytes
) {
    std::map<std::pair<std::string, std::string>, double> oct;

    for (const auto& task : task_order_reverse) {
        for (const auto& host : host_list) {
            auto key = std::make_pair(task, host);

            // CORREÇÃO (fórmula OCT, Eq. 7 de Arabnejad & Barbosa, 2014):
            // OCT(ti,pk) = max_{tj em succ(ti)} { min_{pw} [ OCT(tj,pw) +
            // w(tj,pw) + c_i,j ] } — SEM somar o custo da PRÓPRIA tarefa
            // ti no nível externo. OCT mede o custo do que vem DEPOIS de
            // ti, não de ti mesmo (base: OCT(exit,pk)=0 para todo pk).
            //
            // A versão anterior tinha dois erros que se cancelavam por
            // indução: somava w_ij (custo da própria tarefa) aqui fora, E
            // não somava w(tj,pw) (custo do sucessor) dentro do min. O
            // resultado líquido era OCT_code(t,h) = OCT_paper(t,h) + w(t,h)
            // para todo (t,h) — não gerava NaN/infinito, mas divergia
            // sistematicamente da Eq. 7 e contaminava o rank_oct (Eq. 8)
            // com um viés pelo custo próprio de cada tarefa.
            //
            // Mantida a proteção "if (successors.count(task))": sem ela,
            // successors.at(task) lança std::out_of_range para toda tarefa
            // sem sucessores (todo nó-folha, ex.: tarefas de escrita), o que
            // derrubava a simulação — tratamos "sem sucessores" como lista
            // vazia, igual já é feito no HEFT/IPEFT.
            double max_succ = 0.0;
            if (successors.count(task)) {
                for (const auto& succ : successors.at(task)) {
                    double min_over_pm = std::numeric_limits<double>::max();
                    for (const auto& pm : host_list) {
                        // FRENTE A (parte 1): custo de execução do sucessor,
                        // sensível a host se for tarefa de I/O.
                        double w_succ_pm;
                        if (comp_cost.at(succ) > 0) {
                            w_succ_pm = comp_cost.at(succ) / hosts_capabilities.at(pm);
                        } else if (io_bytes.count(succ) && io_bytes.at(succ) > 0) {
                            w_succ_pm = io_bytes.at(succ) / host_io_bandwidth.at(pm);
                        } else {
                            w_succ_pm = 0.0;
                        }
                        // FRENTE A (parte 2 — a peça que faltava): c_hat
                        // agora implementa a cláusula da Eq. 7 "ci,j = 0 se
                        // pw = pk" — pw é "pm" aqui, pk é "host" (a variável
                        // do laço EXTERNO, o candidato para a tarefa
                        // ATUAL). Antes, c_hat=0 incondicional fazia "host"
                        // nunca aparecer em lugar nenhum da conta, o que
                        // fazia OCT(t,h) sair idêntica para qualquer h —
                        // confirmado por teste isolado. Agora, se o
                        // sucessor ficar no MESMO host que a tarefa atual
                        // (pm==host), a comunicação é grátis; em qualquer
                        // outro host, custa bytes/banda (banda = o menor
                        // entre os dois hosts envolvidos, heurística de
                        // "elo mais lento domina").
                        double bytes = edge_data_bytes.count(std::make_pair(task, succ))
                            ? edge_data_bytes.at(std::make_pair(task, succ)) : 0.0;
                        double c_hat = (pm == host) ? 0.0
                            : bytes / std::min(host_io_bandwidth.at(pm), host_io_bandwidth.at(host));
                        auto succ_key = std::make_pair(succ, pm);
                        min_over_pm = std::min(min_over_pm, oct[succ_key] + w_succ_pm + c_hat);
                    }
                    max_succ = std::max(max_succ, min_over_pm);
                }
            }
            oct[key] = max_succ;  // SEM + w_ij — ver comentário acima
        }
    }
    return oct;
}

int Controller::main() {
    wrench::TerminalOutput::setThisProcessLoggingColor(wrench::TerminalOutput::COLOR_GREEN);
    WRENCH_INFO("Controller starting (PEFT Scheduler)");

    // No WRENCH, o tempo inicial é 0, mas pegamos o tempo atual de forma segura
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
    // FRENTE A: tamanho de cada arquivo em bytes, capturado à parte dos
    // objetos DataFile do WRENCH — necessário para calcular o custo de
    // transferência de tarefas de I/O por host (ver io_bytes mais abaixo).
    // O tamanho NÃO está em file_reads/file_writes (só tem o "id" ali); está
    // aqui, no array "files" do topo do JSON.
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
    // FRENTE A: bytes a transferir por tarefa de I/O — comp_cost continua 0
    // (não é FLOPS), mas io_bytes carrega o tamanho real do arquivo, usado
    // para calcular o tempo de transferência de forma sensível a host (ver
    // host_io_bandwidth mais abaixo). Sem isso, toda tarefa de I/O custava
    // um floor fixo de 0.001s, IGUAL em qualquer host — a mesma causa raiz
    // que fazia a OCT ficar independente de host.
    std::map<std::string, double> io_bytes;
    std::map<std::string, std::vector<std::string>> successors;
    std::map<std::string, std::vector<std::string>> predecessors;
    // FRENTE A (parte 2 — comunicação real por aresta): arquivos que cada
    // tarefa produz/consome, capturados a partir de "uses" (compute tasks,
    // com type input/output) e de file_reads/file_writes (tarefas de I/O).
    // Usado logo abaixo para calcular quantos bytes trafegam em CADA
    // aresta de dependência (ti->tj), cruzando saída de ti com entrada de
    // tj — isso é o que dá substância real ao termo ci,j (Eq. 1) e permite
    // que a cláusula "ci,j=0 se mesmo host" (Eq. 7) finalmente distinga
    // hosts diferentes na OCT.
    std::map<std::string, std::vector<std::string>> task_output_files;
    std::map<std::string, std::vector<std::string>> task_input_files;
    for (auto const &task : data["jobs"]) {
        std::string id = task["id"];
        auto job = job_manager->createCompoundJob(id);
        jobs[id] = job;
        if (task.contains("uses")) {
            for (auto const &u : task["uses"]) {
                std::string file_id = u["id"];
                std::string type = u["type"];
                if (type == "output") task_output_files[id].push_back(file_id);
                else if (type == "input") task_input_files[id].push_back(file_id);
            }
        }
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
    // FRENTE A (parte 2): bytes transferidos em cada aresta de dependência,
    // calculados cruzando os arquivos de SAÍDA da tarefa pai com os
    // arquivos de ENTRADA da tarefa filha. Se não houver arquivo em comum
    // (não deveria acontecer dado o formato do gerador, mas é tratado como
    // 0 por segurança), a aresta é considerada sem custo de comunicação.
    std::map<std::pair<std::string, std::string>, double> edge_data_bytes;
    if (data.contains("dependencies")) {
        for (auto const &dep : data["dependencies"]) {
            for (auto const &p : dep["parents"]) {
                for (auto const &c : dep["children"]) {
                    std::string parent = p;
                    std::string child = c;
                    predecessors[child].push_back(parent);
                    successors[parent].push_back(child);
                    jobs[child]->addParentJob(jobs[parent]);

                    double bytes = 0.0;
                    if (task_output_files.count(parent) && task_input_files.count(child)) {
                        for (auto const &out_f : task_output_files.at(parent)) {
                            for (auto const &in_f : task_input_files.at(child)) {
                                if (out_f == in_f && file_sizes.count(out_f)) {
                                    bytes += file_sizes.at(out_f);
                                }
                            }
                        }
                    }
                    edge_data_bytes[std::make_pair(parent, child)] = bytes;
                }
            }
        }
    }
    // ---------- 4b. Índice topológico (desempate seguro para o rank) ----------
    // CORREÇÃO — falha silenciosa confirmada empiricamente: 66,7% das arestas
    // — 3580 de 5370 — têm rank EXATAMENTE empatado entre pai e filho,
    // porque tarefas de I/O têm peso w=0 na fórmula de rank, herdando o
    // rank do restante da cadeia write->read->tarefa_seguinte. std::sort
    // NÃO é estável (introsort) — em caso de empate, a ordem relativa é
    // arbitrária, podendo colocar um filho ANTES do pai em task_order. Como
    // task_finish_time é um std::map, acessar task_finish_time[p] para um
    // predecessor ainda não processado retorna 0.0 SILENCIOSAMENTE (sem
    // exceção, sem warning) — o algoritmo passa a achar que o predecessor
    // já terminou no tempo zero, distorcendo ready_time sem nenhum aviso.
    // Reproduzido isoladamente em Python: sem este fix, a réplica quebra
    // com KeyError (equivalente ao 0.0 silencioso do C++) já na primeira
    // tentativa. Fix: índice topológico único via Kahn, usado como critério
    // de desempate (ordem estável, secundária) em TODO sort por rank.
    std::map<std::string, int> topo_index;
    {
        std::map<std::string, int> indeg;
        for (auto const &j : jobs) indeg[j.first] = 0;
        for (auto const &pr : predecessors) indeg[pr.first] = (int)pr.second.size();
        std::vector<std::string> queue;
        for (auto const &j : jobs) if (indeg[j.first] == 0) queue.push_back(j.first);
        std::sort(queue.begin(), queue.end());  // determinístico entre execuções
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


    auto hosts_capabilities = bare_metal_compute_service->getCoreFlopRate();
    std::vector<std::string> host_list;
    for (auto const &h : hosts_capabilities) host_list.push_back(h.first);

    // FRENTE A: banda de I/O por host, lida da propriedade io_bandwidth_Bps
    // do platform.xml (bytes/segundo) — mesmo padrão já usado no MOHEFT
    // para carbon_intensity. É isso que faz o custo de uma tarefa de I/O
    // variar de host para host, o que por sua vez é o que faz a OCT (Eq. 7)
    // deixar de ser uma constante independente de host (ver compute_oct).
    std::map<std::string, double> host_io_bandwidth;
    for (const auto& h : host_list) {
        double bw = 1e9;  // fallback conservador: 1 GB/s
        auto sg_host = simgrid::s4u::Host::by_name_or_null(h);
        const char* bw_str = sg_host ? sg_host->get_property("io_bandwidth_Bps") : nullptr;
        if (bw_str) {
            try { bw = std::stod(bw_str); } catch (const std::exception&) {
                WRENCH_WARN("io_bandwidth_Bps inválido no host %s, usando fallback.", h.c_str());
            }
        } else {
            WRENCH_WARN("Host %s sem propriedade io_bandwidth_Bps; usando fallback.", h.c_str());
        }
        host_io_bandwidth[h] = bw;
    }

    // Helper único para tempo de execução de uma tarefa num host — usado
    // tanto em compute_oct quanto na Fase 2, para garantir que os dois
    // usem exatamente a mesma convenção (antes, cada lugar tinha seu
    // próprio floor de 0.001 hardcoded, sem relação com o tamanho real do
    // arquivo nem com a banda do host).
    auto get_exec_time = [&](const std::string& task_id, const std::string& hostname) -> double {
        if (comp_cost.count(task_id) && comp_cost.at(task_id) > 0) {
            return comp_cost.at(task_id) / hosts_capabilities.at(hostname);
        } else if (io_bytes.count(task_id) && io_bytes.at(task_id) > 0) {
            return io_bytes.at(task_id) / host_io_bandwidth.at(hostname);
        }
        return 0.001;  // fallback só para tarefa realmente vazia (não deveria acontecer)
    };

    // ---------- 6. PEFT Fase 1: Prioritização ----------
    double avg_flop_rate = 0.0;
    for (auto const &h : hosts_capabilities) avg_flop_rate += h.second;
    if (!hosts_capabilities.empty()) avg_flop_rate /= hosts_capabilities.size();
    else avg_flop_rate = 1.0;

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

    // ---------- Declaração de task_order movida para aqui (correção do erro de escopo) ----------
    std::vector<std::string> task_order;
    for (auto const &j : jobs) task_order.push_back(j.first);
    std::sort(task_order.begin(), task_order.end(), [&](const std::string &a, const std::string &b) {
        if (std::abs(rank_u[a] - rank_u[b]) > 1e-9) return rank_u[a] > rank_u[b];
        return topo_index[a] < topo_index[b];  // desempate: preserva ordem topológica
    });

    // ---------- Ordem topológica reversa (agora usa task_order declarada acima) ----------
    std::vector<std::string> task_order_reverse = task_order;  // Correção: task_order agora existe
    std::reverse(task_order_reverse.begin(), task_order_reverse.end());  // Ordem reversa para bottom-up

    auto oct_table = compute_oct(comp_cost, successors, host_list, hosts_capabilities, task_order_reverse, io_bytes, host_io_bandwidth, edge_data_bytes);

    // ---------- 7. PEFT Priorização: rank_OCT (mantido só como referência/log) ----------
    std::map<std::string, double> rank_oct;
    for (const auto& task : task_order) {
        double sum_oct = 0.0;
        for (const auto& host : host_list) {
            auto key = std::make_pair(task, host);  // Correção: use make_pair
            sum_oct += oct_table[key];
        }
        rank_oct[task] = sum_oct / host_list.size();  // Média sobre processadores
    }

    // CORREÇÃO (confirmado empiricamente + citação direta do paper original,
    // Arabnejad & Barbosa, Seção 4.2): em grafos largos com muitos jobs de
    // I/O de peso quase-zero, rank_oct (recursão de MÁXIMO sobre sucessores)
    // degenera para valores quase idênticos entre tarefas de entrada — no
    // seu workflow real, o espalhamento de rank_oct entre as 15 primeiras
    // tarefas foi de apenas 0.021, contra 0.128 do rank_u do HEFT no MESMO
    // workflow (6x menos diferenciação). Isso faz a ordem de prioridade do
    // PEFT ser decidida essencialmente por desempate/ruído, não por sinal
    // real. O próprio paper já documentava que rank_u ("ranku") tem
    // desempenho igual ou até ~0.5% MELHOR que rank_oct para a fase de
    // priorização — os autores só usaram rank_oct por economia de cálculo
    // (reaproveitar a tabela OCT), não porque fosse estrategicamente
    // superior: "using ranku would require additional computations without
    // resulting in a significant advantage" (não "sem vantagem" no sentido
    // de ser pior — sem vantagem SUFICIENTE pra justificar o custo extra).
    // A fase de SELEÇÃO DE HOST (OEFT = EFT + OCT) continua inalterada —
    // é ali que o look-ahead de fato importa, conforme o paper.
    std::sort(task_order.begin(), task_order.end(), [&](const std::string &a, const std::string &b) {
        if (std::abs(rank_u[a] - rank_u[b]) > 1e-9) return rank_u[a] > rank_u[b];
        return topo_index[a] < topo_index[b];  // desempate: preserva ordem topológica
    });

    // ---------- 8. PEFT Fase 2: Alocação por OEFT = EFT + OCT (Eq. 9) ----------
    // CORREÇÃO (paridade com HEFT — Algoritmo 1, linha 6 do paper de
    // Arabnejad & Barbosa exige EFT "using insertion-based scheduling
    // policy"): a versão anterior usava host_available_time (escalar único,
    // sempre agenda a próxima tarefa DEPOIS da última), nunca aproveitando
    // um buraco ocioso criado antes no host. O HEFT deste projeto já faz
    // busca de gap via find_insertion_slot/host_schedule — o PEFT tinha uma
    // desvantagem estrutural real, não relacionada ao OCT: perdia toda
    // oportunidade de encaixe que o HEFT aproveitava, sobretudo sob
    // contenção (poucos processadores). A oportunidade de inserção é
    // procurada por host; o critério de escolha do MELHOR host continua
    // sendo OEFT = EFT + OCT (não EFT puro — isso é o que faz ser PEFT).
    std::map<std::string, std::string> scheduling_decision;
    std::map<std::string, double> task_finish_time;
    std::map<std::string, std::vector<std::pair<double, double>>> host_schedule;
    for (auto const &h : hosts_capabilities) {
        host_total_load[h.first] = 0.0;
        host_schedule[h.first] = {};
    }

    auto find_insertion_slot = [&](const std::string &hostname, double ready_time, double duration) -> std::pair<double, double> {
        auto &intervals = host_schedule[hostname];
        double prev_end = 0.0;
        for (auto const &iv : intervals) {
            double gap_start = std::max(prev_end, ready_time);
            double gap_end = iv.first;
            if (gap_end - gap_start >= duration) {
                return {gap_start, gap_start + duration};
            }
            prev_end = iv.second;
        }
        double start = std::max(prev_end, ready_time);
        return {start, start + duration};
    };

    for (auto const &id : task_order) {
        double best_oeft = std::numeric_limits<double>::max();
        double best_eft_for_chosen_host = 0.0;
        double best_est_for_chosen_host = 0.0;
        std::string best_host = "";
        for (auto const &h : hosts_capabilities) {
            std::string hostname = h.first;

            double ready_time = 0.0;
            for (auto const &p : predecessors[id]) {
                double base = task_finish_time[p];
                std::string p_host = scheduling_decision.count(p) ? scheduling_decision.at(p) : hostname;
                double bytes = edge_data_bytes.count(std::make_pair(p, id))
                    ? edge_data_bytes.at(std::make_pair(p, id)) : 0.0;
                double comm = (p_host != hostname && bytes > 0)
                    ? bytes / std::min(host_io_bandwidth.at(p_host), host_io_bandwidth.at(hostname))
                    : 0.0;
                ready_time = std::max(ready_time, base + comm);
            }

            double execution_time = get_exec_time(id, hostname);
            auto slot = find_insertion_slot(hostname, ready_time, execution_time);
            double est = slot.first;
            double eft = slot.second;

            double oct_val = oct_table[std::make_pair(id, hostname)];
            double oeft = eft + oct_val;

            bool strictly_better = oeft < best_oeft - 1e-9;
            bool tied = std::abs(oeft - best_oeft) <= 1e-9;
            bool choose_this_host = strictly_better;
            if (!strictly_better && tied) {
                double current_host_load = 0.0;
                for (auto const &iv : host_schedule[hostname]) current_host_load += (iv.second - iv.first);
                double best_host_load = 0.0;
                for (auto const &iv : host_schedule[best_host]) best_host_load += (iv.second - iv.first);
                choose_this_host = current_host_load < best_host_load;
            }
            if (choose_this_host) {
                best_oeft = oeft;
                best_eft_for_chosen_host = eft;
                best_est_for_chosen_host = est;
                best_host = hostname;
            }
        }
        scheduling_decision[id] = best_host;
        task_finish_time[id] = best_eft_for_chosen_host;

        auto &sched = host_schedule[best_host];
        sched.insert(std::upper_bound(sched.begin(), sched.end(),
                                       std::make_pair(best_est_for_chosen_host, best_eft_for_chosen_host)),
                     std::make_pair(best_est_for_chosen_host, best_eft_for_chosen_host));
    }

    // ---------- 9. Loop de Execução ----------
    std::set<std::string> completed;
    std::set<std::string> submitted;
    std::map<std::string, std::string> actual_execution_host;

    std::map<std::string, double> task_submit_time;
    std::map<std::string, std::vector<std::pair<double, double>>> host_intervals;
    std::map<std::string, double> task_actual_duration; // info bruta p/ CSV
    // Valor REAL de conclusão de cada tarefa, medido no evento de término —
    // separado de task_finish_time (que guarda o EFT do host escolhido na
    // Fase 2 estática, usado para propagar ready_time às sucessoras). Sem
    // risco de contaminar decisões: toda a Fase 2 termina antes de qualquer
    // evento real de conclusão (mesma garantia já validada no HEFT).
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
                service_args[id] = scheduling_decision[id];
                actual_execution_host[id] = scheduling_decision[id];
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
    this->makespan = end_time - start_time;

    // Merge de intervalos por host — evita dupla contagem de tarefas
    // enfileiradas no mesmo host (mesma correção do HEFT/MOHEFT/IPEFT).
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

    // ---------- 10. Processamento de Dados (Corrigido) ----------
    // 10.1. Salva os dados detalhados apenas no CSV com nome dinâmico
    std::string csv_filename = "resultados_" + this->getWorkflowName() + "_PEFT.csv";
    std::ofstream csv_file(csv_filename);
   
    if (csv_file.is_open()) {
        csv_file << "task_id,host,finish_time,load_seconds\n";
        for (auto const &id : task_order) {
            csv_file << id << ","
                     << actual_execution_host[id] << ","
                     << std::fixed << std::setprecision(4) << task_real_finish_time[id] << ","
                     << task_actual_duration[id] << "\n";
        }
        csv_file.close();
        WRENCH_INFO("Log detalhado salvo em %s", csv_filename.c_str());
    }
    // 10.2. Exibe no terminal APENAS o resumo de uso e Makespan
    std::cout << "\n============================================================" << std::endl;
    std::cout << " RESUMO DE UTILIZAÇÃO DA PLATAFORMA" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << std::left << std::setw(20) << "Host" << " | " << "Tempo Total de Uso (s)" << std::endl;
    std::cout << "------------------------------------------------------------" << std::endl;
   
    for (auto const &h : hosts_capabilities) {
        std::cout << std::left << std::setw(20) << h.first << " | " 
                  << std::fixed << std::setprecision(4) << host_total_load[h.first] << " s" << std::endl;
    }
   
    std::cout << "------------------------------------------------------------" << std::endl;
    std::cout << "Makespan Total: " << std::fixed << std::setprecision(4) << (end_time - start_time) << " s" << std::endl;
    std::cout << "============================================================" << std::endl;

    return 0;
} // Fim do main

std::string Controller::getWorkflowName(){
    // Pega o nome do arquivo a partir do caminho completo
    size_t last_slash = json_file_path.find_last_of("/\\");
    std::string name = (last_slash == std::string::npos) ? json_file_path : json_file_path.substr(last_slash + 1);
    
    // Remove a extensão .json
    size_t last_dot = name.find_last_of(".");
    if (last_dot != std::string::npos) name = name.substr(0, last_dot);
    
    return name;
}