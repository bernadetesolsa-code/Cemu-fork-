# Auditoria de crashes — antes da fase 4

Como pedido, parei antes de começar a fase 4 e revisei o código em busca
de bugs que pudessem derrubar o app no celular. Foquei na camada
Android/JNI (`src/android/app/src/main/cpp`), que é onde esse tipo de
problema costuma morar (é a ponte entre Kotlin e o core em C++, e
mexe direto com threads do sistema e com a `Surface` do Android).
Encontrei e corrigi dois problemas reais.

## Bug 1 — `FiberSafeJNICall` criava uma thread do zero a cada chamada

**Onde**: `JNIUtils.h`, usado por `NativeInput.cpp` (vibração de
controle, toda vez que o jogo pede rumble), `NativeSwkbd.cpp` e
`AndroidFilesystemCallbacks.cpp`.

**O que tinha**: a função criava um `std::jthread` novo para rodar a
chamada JNI, mas o objeto era um temporário sem nome — ou seja, o
destrutor rodava (e bloqueava, com `join`) no fim da mesma instrução,
antes até de retornar da função. Isso fazia duas coisas ruins:

1. **Uma thread nativa do sistema operacional inteira criada e
   destruída a cada evento** — cada vibração de controle, cada toque
   na tela. Em jogos que vibram bastante (a maioria dos jogos de Wii U
   com suporte a rumble faz isso o tempo todo), isso é uma tempestade
   de criação de threads, o que é pesado e, em celulares com pouca
   memória ou já sob carga (jogo rodando + app em segundo plano, etc.),
   pode esbarrar no limite de threads do processo.
2. **Se `pthread_create` falhasse** (exatamente nesse cenário de
   esgotamento de recursos), o construtor de `std::jthread` lança
   `std::system_error` — e como não tinha nenhum `try/catch` ao redor,
   essa exceção saía sem ser capturada. Em C++, uma exceção não
   capturada chama `std::terminate()`, que mata o processo na hora.
   Ou seja: exatamente na situação em que o celular já está sob
   pressão (memória/threads baixas), esse código tinha uma chance real
   de crashar o app inteiro, ao invés de simplesmente falhar aquela
   vibração específica.

**Correção**: troquei por uma única thread de trabalho persistente
(`JNIWorkerThread`, um singleton), criada uma vez só, já anexada à
JVM, que processa uma fila de tarefas. `FiberSafeJNICall` agora
enfileira o trabalho e espera ele terminar (mesmo comportamento
síncrono de antes, do ponto de vista de quem chama), só que sem criar
nenhuma thread nova por chamada. Também capturo qualquer exceção
lançada dentro da tarefa com `try/catch` e a repasso pra quem chamou
(`std::rethrow_exception`) só depois que a espera termina, em vez de
deixar escapar sem tratamento dentro da worker thread. Além disso,
adicionei uma proteção contra deadlock: se por acaso alguém chamar
`FiberSafeJNICall` de dentro da própria worker thread (chamada
recursiva), ela executa direto na hora em vez de enfileirar e travar
esperando por si mesma.

## Bug 2 — `setSurface` não verificava ponteiro nulo antes de usar a Surface

**Onde**: `NativeEmulation.cpp`,
`Java_..._NativeEmulation_setSurface`.

**O que tinha**: a assinatura Kotlin já declara o parâmetro como
`Surface?` (nullable) — sinal de que passar `null` é um caso esperado
pelo design. Só que o lado nativo não verificava isso: chamava
`ANativeWindow_fromSurface(env, surface)` direto, e se `surface` for
`null`, isso mexe com o objeto Java por baixo dos panos e pode
derrubar o app com um erro fatal de JNI. Pior: mesmo que
`ANativeWindow_fromSurface` retornasse `nullptr` sem crashar (ex.: a
Surface do lado Java já tinha sido liberada no exato momento dessa
chamada — uma condição de corrida plausível durante rotação de tela ou
o app indo pra segundo plano), o código seguinte chamava
`ANativeWindow_acquire(nullptr)` sem checar, o que é uma
desreferenciação de ponteiro nulo garantida.

Hoje, na prática, o código Kotlin atual (`EmulationViewModel.kt`) nunca
chama `setSurface(null, ...)` — no `surfaceDestroyed` ele usa
`clearPadSurface()`/`pauseTitle()` em vez disso. Ou seja, esse caminho
não está sendo exercitado agora, mas como o próprio contrato Kotlin diz
que pode ser nulo, é só questão de tempo (ou de uma mudança futura na
UI) até alguém chamar dessa forma — e aí seria um crash imediato e
garantido.

**Correção**: adicionei checagem de `surface == nullptr` (limpa o
handle da janela e retorna, sem tentar montar uma `ANativeWindow`) e
checagem do retorno de `ANativeWindow_fromSurface` antes de chamar
`ANativeWindow_acquire`.

## O que revisei e decidi não mexer

Também investiguei a troca de `Surface` durante o jogo (rotação de
tela, app indo pra segundo plano) — é a área clássica de crash em
emuladores Android (usar uma `ANativeWindow` depois que o Android já
destruiu a `Surface` por trás). O código já tem uma sincronização via
`std::atomic<void*>::wait()`/`notify_all()` entre a thread de UI (que
troca a surface) e a thread de renderização Vulkan (`SwapchainInfoVk`),
que só recria a surface quando detecta `surfaceWasLost` e espera o
valor mudar antes de prosseguir. Pareceu uma implementação
propositalmente cuidadosa dos próprios autores, e mexer nela sem
entender toda a lógica de recriação de swapchain do Vulkan tem mais
risco de introduzir um bug novo do que valor — não toquei.

## Verificação

Sem NDK neste ambiente (mesma limitação de sempre), então revisei por
leitura cuidadosa e conferi:

- Balanceamento de chaves/parênteses dos dois arquivos editados
  (`JNIUtils.h`, `NativeEmulation.cpp`) — zero de diferença nos dois.
- Confirmei que os includes novos (`<thread>`, `<mutex>`,
  `<condition_variable>`, `<deque>`, `<functional>`, `<exception>`)
  cobrem tudo que a nova classe usa.
- Chequei todos os pontos que chamam `FiberSafeJNICall`
  (`NativeInput.cpp`, `NativeSwkbd.cpp`,
  `AndroidFilesystemCallbacks.cpp`) para confirmar que nenhum deles
  depende do comportamento antigo de "uma thread nova por chamada"
  (ex.: nenhum guarda um `std::jthread` ou espera reentrância — todos
  só chamam métodos JNI simples).
- Recomendo, ao compilar com o NDK, testar especificamente: girar a
  tela / mandar o app pra segundo plano durante um jogo com rumble
  ativo, e spammar vibração rapidamente, pra validar que não trava nem
  crasha.
