#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app/app.hpp"
#include "apps/monitor/procstat.hpp"

namespace sshos {

// Une ligne de la liste de processus, telle qu'elle est affichée.
struct ProcRow {
  int pid = 0;
  std::string name;
  int cpu_percent = 0;
  uint64_t rss_kb = 0;
};

// LE MONITEUR SYSTÈME.
//
// Le rafraîchissement se déclenche DEPUIS LE DESSIN, au plus une fois par
// seconde. C'est ce qui rend structurelle la règle de la spec -- « un
// moniteur minimisé ne consomme rien » : une fenêtre cachée n'est pas
// dessinée, donc rien n'est lu. Une horloge interne qui échantillonnerait
// toute seule demanderait à l'application de savoir si elle est visible,
// ce que le contrat ne lui dit pas -- et ce qu'elle finirait par deviner
// de travers.
class Monitor : public App {
 public:
  Monitor();

  void attach(Host& host) override;
  void render(View v) override;
  void on_key(const KeyEvent& k) override;
  Size min_size() const override { return {30, 8}; }

  // L'ordre de tri. Par CPU au départ : c'est la question qu'on se pose en
  // ouvrant un moniteur.
  enum class Sort { Cpu, Memory };

  // --- pour les tests ---
  // L'échantillonnage prend le TEXTE des trois fichiers et la liste des
  // processus déjà lus : c'est ce qui permet de décrire une machine à
  // douze cœurs sous charge sans en avoir une. `now_ms` est l'horloge de
  // l'appelant -- le dessin passe la sienne.
  void sample_for_tests(int64_t now_ms, std::string_view stat,
                        std::string_view meminfo, std::string_view loadavg,
                        const std::vector<ProcInfo>& procs);
  const std::vector<int>& cores_for_tests() const { return core_percent_; }
  const std::vector<ProcRow>& rows_for_tests() const { return rows_; }
  Sort sort_for_tests() const { return sort_; }
  int64_t last_sample_for_tests() const { return last_ms_; }
  bool has_delta_for_tests() const { return has_prev_; }
  // Empêche le dessin de relire `/proc`. Sans ça, un cas qui décrit une
  // machine imaginaire la verrait écrasée par la vraie au premier rendu.
  void freeze_for_tests() { frozen_ = true; }

 private:
  void apply_sample(int64_t now_ms, std::string_view stat,
                    std::string_view meminfo, std::string_view loadavg,
                    const std::vector<ProcInfo>& procs);
  void resort();

  std::vector<CpuTimes> prev_cpu_;
  std::vector<ProcInfo> prev_procs_;
  bool has_prev_ = false;
  int64_t last_ms_ = 0;
  bool sampled_once_ = false;
  bool frozen_ = false;

  std::vector<int> core_percent_;  // le total est l'indice 0
  MemInfo mem_;
  std::vector<int> load_;
  std::vector<ProcRow> rows_;
  Sort sort_ = Sort::Cpu;
  Host* host_ = nullptr;
};

}  // namespace sshos
