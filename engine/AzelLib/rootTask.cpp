#include "PDS.h"

extern bool pdForceTaskGraphDump;

// Port diagnostic: PD_START_SCENE=field|town|battle|continue|menu skips the
// title and boots straight into that scene. Reaching a scene by scripted button
// presses is unreliable on the handheld (the menu opens at an unpredictable
// wall-clock time at single-digit fps), and this is the only way to check the
// field renderer without a human in the loop.
extern p_workArea createFieldDebugTask(p_workArea);
extern p_workArea createTownDebugTask(p_workArea);
extern p_workArea createBattleDebugTask(p_workArea);
extern p_workArea createContinueTask(p_workArea);
extern p_workArea createNewGameTask(p_workArea);

static p_workArea(*pdSceneFromEnv())(p_workArea)
{
    const char* e = getenv("PD_START_SCENE");
    if (e == nullptr) return nullptr;
    if (strcmp(e, "field") == 0) return createFieldDebugTask;
    if (strcmp(e, "town") == 0) return createTownDebugTask;
    if (strcmp(e, "battle") == 0) return createBattleDebugTask;
    if (strcmp(e, "continue") == 0) return createContinueTask;
    // Exactly what the title menu's NEW GAME entry calls: the real story path
    // (module manager -> intro -> name entry -> field), not a debug harness.
    if (strcmp(e, "newgame") == 0) return createNewGameTask;
    return nullptr;
}

struct s_initialTaskWorkArea : public s_workAreaTemplate<s_initialTaskWorkArea>
{
    static TypedTaskDefinition* getTypedTaskDefinition()
    {
        static TypedTaskDefinition taskDefinition = { &s_initialTaskWorkArea::Init, NULL, &s_initialTaskWorkArea::Draw, NULL };
        return &taskDefinition;
    }

    static void Init(s_initialTaskWorkArea*);
    static void Draw(s_initialTaskWorkArea*);

    u32 m_state;
    p_workArea m_4;
};

s_initialTaskStatus initialTaskStatus;

// 06024b56
void resetNamesForNewGame() {
    allocateGlobalStringTable(3);
    setGlobalStringTableEntry(0, mainGameState.gameStats.m94_playerName);
    setGlobalStringTableEntry(1, mainGameState.gameStats.mA5_dragonName);
    setGlobalStringTableEntry(2, "");
}

void s_initialTaskWorkArea::Init(s_initialTaskWorkArea* pWorkArea)
{
    pWorkArea->m_state = 0;
    pWorkArea->m_4 = 0;

    initialTaskStatus.m_currentTask = 0;
    if (auto* scene = pdSceneFromEnv())
    {
        initialTaskStatus.m_pendingTask = scene;
    }
    else if (initialTaskStatus.mC_showWarningTask)
    {
        initialTaskStatus.m_pendingTask = startLoadWarningTask;
    }
    else
    {
        initialTaskStatus.m_pendingTask = createTitleScreenTask;
    }

    resetNamesForNewGame();
}
void s_initialTaskWorkArea::Draw(s_initialTaskWorkArea* pWorkArea)
{
    switch (pWorkArea->m_state)
    {
    case 0: // init state
            // Some sound init code here
        pWorkArea->m_state++;
        [[fallthrough]];
    case 1: // wait for new task
        if (!initialTaskStatus.m_pendingTask)
            return;
        if (initialTaskStatus.m_pendingTask != initialTaskStatus.m_currentTask)
        {
            if (pWorkArea->m_4)
            {
                s_task* pTask = pWorkArea->m_4->getTask();
                pTask->m14_flags |= 1;
            }

            initialTaskStatus.m_previousTask = initialTaskStatus.m_currentTask;
            initialTaskStatus.m_currentTask = initialTaskStatus.m_pendingTask;
            pWorkArea->m_state = 2;
        }
        initialTaskStatus.m_pendingTask = NULL;
        break;
    case 2: // start new task
        // Port diagnostic: every scene change goes through here, so naming the
        // function pointer make a stuck screen attributable to one scene.
        if (getenv("PD_TRACE_TASKGRAPH"))
            fprintf(stderr, "[scene] start %p (previous %p)\n",
                    (void*)initialTaskStatus.m_currentTask,
                    (void*)initialTaskStatus.m_previousTask);
        pWorkArea->m_4 = initialTaskStatus.m_currentTask(pWorkArea);
        pdForceTaskGraphDump = true;
        pWorkArea->m_state = 1;
        break;
    default:
        assert(0);
    }
}

void startInitialTask()
{
    createRootTask<s_initialTaskWorkArea>();
}
