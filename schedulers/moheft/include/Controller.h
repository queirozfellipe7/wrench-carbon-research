#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <wrench-dev.h>
#include <map>
#include <vector>
#include <string>

class Controller : public wrench::ExecutionController {
public:
    Controller(const std::shared_ptr<wrench::BareMetalComputeService> &bare_metal_compute_service,
               const std::shared_ptr<wrench::SimpleStorageService> &storage_service,
               const std::string &hostname,
               const std::string &json_file_path);

    int main() override;
    std::string getWorkflowName();

    // Expõe a carga real (segundos de uso, medida de verdade — não estimada)
    // por host, e o makespan, para o Simulator.cpp reportar a utilização
    // real da plataforma.
    std::map<std::string, double> getHostTotalLoad() const { return host_total_load; }
    double getMakespan() const { return makespan; }

private:
    std::shared_ptr<wrench::BareMetalComputeService> bare_metal_compute_service;
    std::shared_ptr<wrench::SimpleStorageService> storage_service;
    std::string json_file_path;

    // Preenchidos ao final de main() com dados reais medidos da execução
    std::map<std::string, double> host_total_load;
    double makespan = 0.0;

    // Estruturas necessárias para MOHEFT
    struct Solution {
        std::map<std::string, std::string> assignment;      // tarefa → host
        double makespan = 0.0;
        double energy_joules = 0.0;
        double carbon_gco2 = 0.0;
        std::map<std::string, double> finish_time;          // tempo de término por tarefa
        std::map<std::string, double> host_avail;           // disponibilidade futura por host
        double crowding_distance = 0.0;                      // usado só na poda da fronteira por K (diversidade nos 3 objetivos)
    };

    bool dominates(const Solution& a, const Solution& b) const;
    Solution select_best_solution(const std::vector<Solution>& pareto_front) const;
};

#endif // CONTROLLER_H