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

int Controller::main() {
    wrench::TerminalOutput::setThisProcessLoggingColor(wrench::TerminalOutput::COLOR_GREEN);
    WRENCH_INFO("Controller starting (HEFT Scheduler with Metrics)");

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
    // objetos DataFile do WRENCH — mesmo padrão já validado no PEFT/IPEFT.
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
    // FRENTE A: bytes a transferir por tarefa de I/O, e arquivos de
    // entrada/saída de cada tarefa (via "uses" para tarefas de computação,
    // file_reads/file_writes para tarefas de I/O) — usados logo abaixo
    // para calcular edge_data_bytes (dado transferido em cada aresta do
    // DAG), mesmo padrão já validado no PEFT/IPEFT. Antes desta correção,
    // toda tarefa de I/O custava um floor fixo de 0.001s na decisão do
    // HEFT, IGUAL em qualquer host, não importa o tamanho real do arquivo
    // — o HEFT decidia com uma informação de custo pior que a do PEFT/
    // IPEFT, uma desvantagem metodológica injusta na comparação entre eles.
    std::map<std::string, double> io_bytes;
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
    // FRENTE A: bytes transferidos em cada aresta, cruzando saída do pai
    // com entrada do filho — mesmo padrão já validado no PEFT/IPEFT.
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

    // ---------- 5. HEFT Fase 1: Prioritização ----------
    auto hosts_capabilities = bare_metal_compute_service->getCoreFlopRate();
    std::vector<std::string> host_list;
    for (auto const &h : hosts_capabilities) host_list.push_back(h.first);

    // FRENTE A: banda de I/O por host, lida da propriedade io_bandwidth_Bps
    // do platform.xml — mesmo padrão já validado no PEFT/IPEFT.
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

    // Helper único para tempo de execução de uma tarefa num host.
    auto get_exec_time = [&](const std::string& task_id, const std::string& hostname) -> double {
        if (comp_cost.count(task_id) && comp_cost.at(task_id) > 0) {
            return comp_cost.at(task_id) / hosts_capabilities.at(hostname);
        } else if (io_bytes.count(task_id) && io_bytes.at(task_id) > 0) {
            return io_bytes.at(task_id) / host_io_bandwidth.at(hostname);
        }
        return 0.001;
    };

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

    std::vector<std::string> task_order;
    for (auto const &j : jobs) task_order.push_back(j.first);
    std::sort(task_order.begin(), task_order.end(), [&](const std::string &a, const std::string &b) {
        return rank_u[a] > rank_u[b];
    });

    // ---------- 6. HEFT Fase 2: Seleção de Recursos (insertion-based) ----------
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
        double best_eft = std::numeric_limits<double>::max();
        double best_est = 0.0;
        std::string best_host = "";

        for (auto const &h : hosts_capabilities) {
            std::string hostname = h.first;

            // FRENTE A (parte 2): ready_time agora é calculado POR HOST
            // candidato, considerando de qual host cada predecessora saiu
            // e o custo de comunicação real até este host (0 se mesmo
            // host) — mesma correção já validada no PEFT/IPEFT. Antes,
            // ready_time era calculado uma vez por tarefa, como se o dado
            // da predecessora chegasse instantaneamente em qualquer host.
            double ready_time = 0.0;
            for (auto const &p : predecessors[id]) {
                double base = task_finish_time[p];
                std::string p_host = scheduling_decision.count(p) ? scheduling_decision.at(p) : hostname;
                double bytes = edge_data_bytes.count(std::make_pair(p, id))
                    ? edge_data_bytes.at(std::make_pair(p, id)) : 0.0;
                // MODELO P×P: custo de comunicação usa a banda do LINK entre
                // o host do predecessor e este host candidato. Zero se mesmo
                // host (bandwidth_between devolve ~infinito). É isto que dá ao
                // look-ahead (PEFT/IPEFT) algo a otimizar: alocar tarefas
                // dependentes em GPUs bem conectadas reduz a comunicação.
                double comm = (p_host != hostname && bytes > 0)
                    ? bytes / std::min(host_io_bandwidth.at(p_host), host_io_bandwidth.at(hostname))
                    : 0.0;
                ready_time = std::max(ready_time, base + comm);
            }

            // FRENTE A (parte 1): execution_time agora sensível a host se
            // for tarefa de I/O, via get_exec_time — antes usava floor
            // fixo de 0.001s, igual em qualquer host.
            double execution_time = get_exec_time(id, hostname);

            auto slot = find_insertion_slot(hostname, ready_time, execution_time);
            double est = slot.first;
            double eft = slot.second;

            bool strictly_better = eft < best_eft - 1e-9;
            bool tied = std::abs(eft - best_eft) <= 1e-9;
            bool choose_this_host = strictly_better;
            if (!strictly_better && tied) {
                double current_host_load = 0.0;
                for (auto const &iv : host_schedule[hostname]) current_host_load += (iv.second - iv.first);
                double best_host_load = 0.0;
                for (auto const &iv : host_schedule[best_host]) best_host_load += (iv.second - iv.first);
                choose_this_host = current_host_load < best_host_load;
            }
            if (choose_this_host) {
                best_eft = eft;
                best_est = est;
                best_host = hostname;
            }
        }

        scheduling_decision[id] = best_host;
        task_finish_time[id] = best_eft;

        auto &sched = host_schedule[best_host];
        sched.insert(std::upper_bound(sched.begin(), sched.end(),
                                       std::make_pair(best_est, best_eft)),
                     std::make_pair(best_est, best_eft));
    }

    // ---------- 7. Loop de Execução ----------
    std::set<std::string> completed;
    std::set<std::string> submitted;
    std::map<std::string, std::string> actual_execution_host;
    std::map<std::string, double> task_submit_time;
    std::map<std::string, std::vector<std::pair<double, double>>> host_intervals;
    std::map<std::string, double> task_actual_duration;
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

    for (auto &entry : host_intervals) {
        auto &intervals = entry.second;
        if (intervals.empty()) continue;

        std::sort(intervals.begin(), intervals.end());

        double merged_total = 0.0;
        double cur_start = intervals[0].first;
        double cur_end   = intervals[0].second;

        for (size_t i = 1; i < intervals.size(); ++i) {
            double s = intervals[i].first;
            double e = intervals[i].second;
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

    // ---------- 8. Processamento de Dados ----------
    std::string csv_filename = "resultados_" + this->getWorkflowName() + "_HEFT.csv";
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

    std::cout << "\n============================================================" << std::endl;
    std::cout << "           RESUMO DE UTILIZAÇÃO DA PLATAFORMA " << std::endl;
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
}

std::string Controller::getWorkflowName(){
    size_t last_slash = json_file_path.find_last_of("/\\");
    std::string name = (last_slash == std::string::npos) ? json_file_path : json_file_path.substr(last_slash + 1);
    size_t last_dot = name.find_last_of(".");
    if (last_dot != std::string::npos) name = name.substr(0, last_dot);
    return name;
}