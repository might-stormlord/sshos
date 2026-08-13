#pragma once

#include <cstddef>
#include <string>

#include "app/app.hpp"
#include "apps/editor/buffer.hpp"

namespace sshos {

// L'ÉDITEUR. Le tampon en dessous, le curseur et les gestes ici.
//
// `Ctrl+Q` N'ARRIVERA JAMAIS : le bureau l'intercepte pour détacher
// (§7.4), et l'éditeur ne le verrait donc pas. C'est `Ctrl+X` qui quitte.
// Contrepartie assumée d'un geste de détachement sans accord.
//
// Pas de coloration syntaxique : elle arrive en dernier, et on peut déjà
// lancer `vim` dans une fenêtre Terminal.
class Editor : public App {
 public:
  Editor();
  explicit Editor(std::string path);

  void attach(Host& host) override;
  void render(View v) override;
  void on_key(const KeyEvent& k) override;
  void on_resize(Size s) override;
  bool wants_cursor(Pos& out) const override;
  Size min_size() const override { return {24, 6}; }
  CloseCheck can_close() const override;

  // Ce que l'éditeur est en train de faire.
  enum class Mode { Normal, Searching, Confirming };

  // --- pour les tests ---
  const TextBuffer& buffer_for_tests() const { return buf_; }
  TextPos cursor_for_tests() const { return cur_; }
  Mode mode_for_tests() const { return mode_; }
  const std::string& status_for_tests() const { return status_; }
  size_t top_for_tests() const { return top_; }

 private:
  void settle();
  void save();
  int rows_for_text() const;

  TextBuffer buf_;
  TextPos cur_{};
  size_t top_ = 0;
  std::string path_;
  std::string status_;
  std::string query_;
  Mode mode_ = Mode::Normal;
  Size size_{60, 20};
  Host* host_ = nullptr;
};

}  // namespace sshos
