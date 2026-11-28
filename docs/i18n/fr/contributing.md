# Leonida : contribuer (`fr`)

> [!NOTE]
> La version anglaise de [`CONTRIBUTING.md`](../../../CONTRIBUTING.md) **fait foi**. Cette traduction est
> synchronisée avec le commit `364754de`. En cas de différence, suivez l'anglais.

Merci de votre aide. Quelques règles permettent au projet de rester hébergeable :

- **Aucune donnée du jeu, jamais.** Cela comprend les archives, l'exécutable, les fichiers extraits, les shaders
  (DXBC, FSSM, désassemblage, SPIR-V), les textures, les modèles, l'audio, les vidéos, ainsi que les rapports ou
  journaux qui les citent. Le `.gitignore` bloque les types courants ; vérifiez quand même `git status` avant de
  commiter.
- **Pas de clés, de déchiffrement ni de contournement de protection**, et aucun lien vers un endroit où les obtenir.
  Les outils ne lisent que des fichiers déjà en clair sur le disque.
- De courts extraits nécessaires pour expliquer un format (quelques octets d'en-tête, une séquence d'instructions)
  sont acceptés dans la documentation et les commentaires.

## Avant de commencer

Réservez la tâche dans [l'issue de coordination][claims] : l'issue ouverte intitulée « Coordinating who works on
what ». Quand le fil devient trop long, elle est remplacée par une nouvelle issue, c'est pourquoi le lien lance une
recherche. Consultez ses tableaux et les pull requests ouvertes, publiez `claiming: <ID>` dans l'issue (ou une
description d'une ligne pour une tâche qui n'y figure pas encore), puis indiquez l'identifiant dans votre pull
request.

Vous avez repéré quelque chose à faire mais vous ne comptez pas vous en charger ? Publiez `finding: <one line>` dans
l'issue, en précisant où se trouve le problème et pourquoi il compte. La tâche rejoint alors la liste pour quelqu'un
d'autre. Si deux pull requests se recoupent malgré tout, celle qui arrive en premier est fusionnée et l'autre doit
être rebasée ou fermée.

Une fois la pull request ouverte, publiez `done: <ID> -> #<PR>` dans l'issue. Les pull requests visent `main`.

Le dépôt d'origine (`ItsNotRexed/Leonida`) est devenu privé le 23 novembre 2026, avec ses issues et ses pull
requests. Un `#N` écrit avant cette date dans la documentation, un commentaire de code ou un message de commit est
un **ancien** numéro et ne mène plus nulle part, y compris les deux vers lesquels cette page renvoyait : #166
(résultats des tests GPU) et #313 (la page de la couche D3D12 → Vulkan).

- Les titres des issues qui y étaient ouvertes sont répertoriés dans
  [#18](https://github.com/ubiquitarianism/leonida/issues/18). Pour en reprendre une, rouvrez-la ici avec un lien
  vers cet index.
- Les pull requests qui y étaient ouvertes sont conservées dans des branches `old-pr/<ancien numéro>`, listées dans
  [#17](https://github.com/ubiquitarianism/leonida/issues/17). Pour en reprendre une, rebasez-la sur `main`,
  gardez les commits de son auteur et ouvrez une pull request ici.
- Un résultat de test GPU est une pull request qui ajoute un fichier dans
  [`gpu-test-results/`](../../../gpu-test-results), au format des fichiers qui s'y trouvent ;
  [docs/testing.md](../../testing.md) explique comment lancer les tests.
- Le plan et l'état de la couche D3D12 → Vulkan sont dans [docs/d3d12_vulkan.md](../../d3d12_vulkan.md).

[claims]: https://github.com/ubiquitarianism/leonida/issues?q=is%3Aissue%20is%3Aopen%20in%3Atitle%20%22Coordinating%20who%20works%20on%20what%22

## Travailler sur le code

- Les outils Python se lancent avec `python -I` et n'utilisent que la bibliothèque standard, sauf si le fichier
  indique le contraire.
- Un fichier texte écrit par un outil (JSON, CSV, manifeste, listing d'assembleur) précise sa fin de ligne :
  `open(path, "w", encoding="utf-8", newline="\n")` (de même pour `Path.open`, `Path.write_text` et un
  `tempfile.NamedTemporaryFile` en mode texte). La sortie est ainsi en LF même sous Windows, et une même entrée
  donne les mêmes octets sur toutes les plateformes. `tests/test_text_write_line_endings.py` échoue si un outil
  l'oublie.
- Un générateur de rapport refuse une sortie qui désigne l'une de ses entrées ou une autre de ses sorties. Pour le
  vérifier, il appelle `tools/report_paths.same_report_path(a, b)` au lieu de refaire le calcul lui-même : la
  fonction résout les deux chemins, applique les règles de casse du volume et se rabat sur `os.path.samefile`. Un
  nouvel outil réutilise donc cette réponse plutôt que d'en garder sa propre copie. `tests/test_report_paths.py`
  couvre ce cas.
- `tools/rpf9dump` a besoin du sous-module : `git submodule update --init`.
- Une nouvelle règle du traducteur de shaders va dans le tableau de contrat de `docs/design.md`, avec la façon dont
  elle a été confirmée : un test GPU, une référence Mesa/LLVM ou un comptage sur le corpus.
- `python -I tests/test_<name>.py` lance un test unitaire (bibliothèque standard uniquement ; la CI les lance tous).
  Les fichiers de test sans bloc `__main__`, ou qui contiennent un `import pytest` (ils ont besoin des paquets de
  `tests/requirements.txt`), se lancent plutôt avec `python -m pytest tests/test_<name>.py`, comme l'explique
  [AGENTS.md](../../../AGENTS.md). Avec `python -I`, ils échouent à l'import ou ne lancent rien.
- Tests du runtime : `cmake -S runtime -B runtime/build`, `cmake --build runtime/build --config RelWithDebInfo`,
  puis `ctest --test-dir runtime/build -C RelWithDebInfo --output-on-failure`. Les tests CPU qui ne demandent aucune
  donnée du jeu sont configurés sous Windows ; Linux et macOS configurent le sous-ensemble portable (`gta6rt` et les
  shims n'existent que pour Windows). Les tests qui ont besoin du paquet commercial le lisent sur place via
  `LEONIDA_GAME_DIR` (et `LEONIDA_MOVIE` pour le test de décodage Bink), et affichent `SKIP` quand la variable n'est
  pas définie. Ne copiez et ne commitez jamais ces fichiers.
- Tests du streaming du viewer : `node --test tests/test_viewer_streaming.cjs` (Node.js 22+, la CI teste la 24 LTS ;
  ni paquets, ni navigateur, ni réseau, ni données du jeu). Ils exécutent le code de streaming du viewer avec un
  chargeur et une horloge synthétiques.
- `loader_test` écrit de petites images PE dans un dossier `leonida_loader_test` de `%TEMP%`. Si un antivirus bloque
  les exécutables tout juste écrits, le test s'arrête en donnant cette raison. Excluez le dossier, ou faites pointer
  `LEONIDA_TEST_DIR` vers un dossier que l'antivirus ignore.
- Vérification des outils : `python -m compileall -q make_game.py tools tests`. L'outil facultatif
  `vk_pipeline_check <spirv dir> [cache.bin] [--jobs N]` crée un module (et, pour les shaders compute, un pipeline)
  pour chaque shader traduit, puis signale les échecs et le chemin du cache enregistré.
- Les tests GPU sont de petits programmes RDNA2 écrits à la main, dans `runtime/test/shaders`. `build.py` les
  convertit en SPIR-V avec `llvm-mc` et la cible AMDGPU (LLVM 21+), puis `check.py` les exécute sur votre GPU et
  compare les résultats. Un nouveau test donne ses valeurs attendues dans le commentaire en tête du fichier, et
  reçoit sa vérification dans `check.py`. [docs/testing.md](../../testing.md) décrit l'installation pour chaque
  système, y compris les options natives sous Windows, ainsi que les problèmes connus.
- Dans la pull request, indiquez ce que vous avez testé et sur quel GPU.
- Lancez les tests du traducteur avec `python -I scripts/run_semantic_tests.py tests/semantic` (voir
  `tests/semantic/README.md`). Une nouvelle règle du traducteur doit venir avec une petite fixture `.asm` écrite à la
  main et les mots de sortie attendus.

## Pull requests

- Un seul sujet par pull request, basée sur la version actuelle de `main`. La suite du travail va dans une nouvelle
  pull request, pas dans une pull request déjà ouverte.
- Remplissez le [modèle](../../../.github/pull_request_template.md) : la réservation, `Depends on:`, `Fixes: #N` pour
  une issue qu'elle ferme, ce que vous avez testé, `GPUs tested:` (fabricant, modèle, pilote et système) et si elle a
  été faite avec l'aide d'une IA.
- Une pull request en conflit avec `main` reçoit l'étiquette `needs rebase` (`tools/pr_conflict_label.py`, lancé à
  chaque push sur `main` et toutes les heures). Rebasez-la vous-même sur `main` ; l'étiquette disparaît dans l'heure
  une fois qu'elle fusionne proprement. Les relectures commencent par les pull requests qui fusionnent proprement.
