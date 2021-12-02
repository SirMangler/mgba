/*
	Authored By SirMangler
	Date: 22 Nov 2021
*/
extern "C" {
#include <mgba/internal/gba/redbirden.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/core/core.h>
#include <mgba-util/elf-read.h>
#include <mgba-util/vfs.h>
#include <mgba/internal/gba/input.h>
}

#include <vector>
#include <queue>
#include <iostream>
#include <fstream> 
#include <chrono>
#include <map>

// Junk data, can be overwritten
#define MODLOADER_START 0x08720000

struct CodeChange {
	uint32_t address;
	uint16_t var;
};

enum TaskType {
	None = -1,
	CustomWild = 0,
	StatusAll = 1,
};

struct Task {
	uint32_t taskId = 0;
	uint16_t type = TaskType::None;
	uint8_t data[32];
}; // size 0x28

Task activeTask;
Task priorityTask;

uint32_t CurrentTaskId_Addr = 0x203FFF4;
uint32_t LastTaskId_Addr = 0x203FFF8;
uint32_t nextTask_Addr = 0x203FFFC;
uint32_t priorityTask_Addr = 0x2040028;

struct NextWild {
	unsigned short species;
	unsigned char name[10];
};

std::queue<Task> task_queue;
uint32_t taskId = 0;

// This task could happen randomly, so we need to be prepared to execute it immediately
std::queue<NextWild> wild_queue;
int state_update = -1;

static std::vector<CodeChange> code_changes;
static bool mod_initialised = false;
uint32_t mod_entrypoint = 0;

std::map<unsigned char, uint8_t> charmap = { 
 {' ', 0x00}, {'=', 0x35}, {'&', 0x2D}, {'+', 0x2E}, {'0', 0xA1}, {'1', 0xA2}, 
 {'2', 0xA3}, {'3', 0xA4}, {'4', 0xA5}, {'5', 0xA6}, {'6', 0xA7}, {'7', 0xA8}, 
 {'8', 0xA9}, {'9', 0xAA}, {'!', 0xAB}, {'?', 0xAC}, {'.', 0xAD}, {'-', 0xAE},
 {'\'', 0xB}, {',', 0xB8}, {'/', 0xBA}, {'A', 0xBB}, {'B', 0xBC}, {'C', 0xBD}, 
 {'D', 0xBE}, {'E', 0xBF}, {'F', 0xC0}, {'G', 0xC1}, {'H', 0xC2}, {'I', 0xC3}, 
 {'J', 0xC4}, {'K', 0xC5}, {'L', 0xC6}, {'M', 0xC7}, {'N', 0xC8}, {'O', 0xC9}, 
 {'P', 0xCA}, {'Q', 0xCB}, {'R', 0xCC}, {'S', 0xCD}, {'T', 0xCE}, {'U', 0xCF}, 
 {'V', 0xD0}, {'W', 0xD1}, {'X', 0xD2}, {'Y', 0xD3}, {'Z', 0xD4}, {'a', 0xD5}, 
 {'b', 0xD6}, {'c', 0xD7}, {'d', 0xD8}, {'e', 0xD9}, {'f', 0xDA}, {'g', 0xDB},
 {'h', 0xDC}, {'i', 0xDD}, {'j', 0xDE}, {'k', 0xDF}, {'l', 0xE0}, {'m', 0xE1},
 {'n', 0xE2}, {'o', 0xE3}, {'p', 0xE4}, {'q', 0xE5}, {'r', 0xE6}, {'s', 0xE7}, 
 {'t', 0xE8}, {'u', 0xE9}, {'v', 0xEA}, {'w', 0xEB}, {'x', 0xEC}, {'y', 0xED}, 
 {'z', 0xEE}, {':', 0xF0}
};

template <typename T>
T swap(T num) {
	if (sizeof(num) < 2)
		return num;

	return (num >> 8) | (num << 8);
}

uint32_t swap(uint32_t num) {
	return (num >> 8) | (num << 8);
}

uint16_t swap(uint16_t num) {
	return (num >> 8) | (num << 8);
}

void writeTask(struct mCore* core, uint32_t address, Task t) {
	core->busWrite32(core, address, t.taskId);
	core->busWrite16(core, address + 0x4, t.type);

	for (int i = 0; i < 32; i++) {
		core->busWrite8(core, address + 0x6 + i, t.data[i]);
	}
}

// Update modstates
void RedBirden_RunMods(struct mCore* core) {
	// Check revision/game before executing
	if (core->busRead32(core, 0x080000B0) != 0x00963130)
		return;

	if (!mod_initialised) {
		RedBirden_InitMods(core);
	}

	if (state_update > -1) {
		core->busWrite32(core, 0x02037078 /* PlayerAvatar 0*/, 
			state_update ? 1 << 2 /* Mach Bike */ : 1 << 0 /* On Foot */);

		state_update = -1;
	}

	// Ensure the code changes are applied
	for (auto& code_change : code_changes) {
		core->rawWrite16(core, code_change.address, 8, code_change.var);
	}

	uint32_t finished_task = core->busRead16(core, LastTaskId_Addr);

	if (priorityTask.taskId) {
		if (priorityTask.taskId != finished_task) {
			if (core->busRead16(core, priorityTask_Addr) != priorityTask.taskId) {
				writeTask(core, priorityTask_Addr, priorityTask);
				mLog(_mLOG_CAT_GBA_DEBUG, mLogLevel::mLOG_DEBUG, "Running Priority Task: %x", priorityTask.taskId);
			}
		}
		else {
			core->busWrite32(core, priorityTask_Addr, 0); // clear task from being re-ran.
			mLog(_mLOG_CAT_GBA_DEBUG, mLogLevel::mLOG_DEBUG, "Priority task finished");
		}
	}
	
	if (!activeTask.taskId || activeTask.taskId == finished_task) {
		if (task_queue.empty()) {
			activeTask.taskId = 0;
			return;
		}

		activeTask = task_queue.front();
		activeTask.taskId = taskId++;
		task_queue.pop();

		writeTask(core, nextTask_Addr, activeTask);

		mLog(_mLOG_CAT_GBA_DEBUG, mLogLevel::mLOG_DEBUG, "Running Task: %x", activeTask.taskId);
	}
}

void add_code_change(uint32_t address, uint16_t var) {
	CodeChange c;
	c.address = address;
	c.var = swap(var);

	code_changes.emplace_back(c);
}

void RedBirden_InitMods(struct mCore* core) {
	/* Hook - Thumb */

	uint16_t original_instructions[10];
	for (int i = 0; i < 10; i++) {
		original_instructions[i] = swap(core->busRead16(core, 0x08007A12 + (i * 0x2))); // swap the swap be like
	}

	// PUSH    {R4-R7,LR}
	add_code_change(0x08007A12, 0xFFB4); // push { r0, r1, r2, r3, r4, r5, r6, r7 }
	add_code_change(0x08007A14, 0x0820); // mov r0, #0x08
	add_code_change(0x08007A16, 0x0006); // lsl r0, r0, #24
	add_code_change(0x08007A18, 0x7221); // mov r1, #0x72
	add_code_change(0x08007A1A, 0x0904); // lsl r1, r1, #16
	add_code_change(0x08007A1C, 0x0843); // orr r0, r1 ; Load 0x08720000
	add_code_change(0x08007A1E, 0x0121); // mov r1, #0x1
	add_code_change(0x08007A20, 0x4018); // add r0, r1 ; Load 0x08720001
	add_code_change(0x08007A22, 0xFE46); // mov lr, pc ; move new return position
	add_code_change(0x08007A24, 0x0047); // bx r0

	/* ModLoader - Thumb - 0x08720000 */
	add_code_change(MODLOADER_START + 0x0, 0x00B5); // push { lr } ; push return
	// 0x2 <- Execute Mod Start ->	
	add_code_change(MODLOADER_START + 0x4, 0x01BC); // pop { r0 } ; contains LR, implicitly saved by next function.
	add_code_change(MODLOADER_START + 0x6, 0x0130); // add r0, #0x1
	add_code_change(MODLOADER_START + 0x8, 0x8646); // mov lr, r0
	add_code_change(MODLOADER_START + 0xA, 0xFFBC); // pop { r0, r1, r2, r3, r4, r5, r6, r7 }

	/* Run replaced instructions */
	for (int i = 0; i < 10; i++) {
		add_code_change(MODLOADER_START + 0xC + (0x2 * i), original_instructions[i]);
	} // Size: 0x14


	add_code_change(MODLOADER_START + 0x20, 0x7047); // bx lr ; Return

	char path[PATH_MAX];
	mCoreConfigDirectory(path, PATH_MAX);
	strncat(path, "/redmod.elf", PATH_MAX - strlen(path));

	std::ifstream f (path, std::ifstream::binary);

	if (!f.is_open()) {
		mLog(_mLOG_CAT_GBA_DEBUG, mLogLevel::mLOG_DEBUG, "Could not open mod file: %s", strerror(errno));
	}

	f.seekg(0x18, std::ios::beg);
	f.read((char*) &mod_entrypoint, sizeof(uint32_t));

	f.seekg(0x100, std::ios::beg);

	uint32_t address = 0x08720100;
	while (f.good()) {
		uint16_t val;
		f.read((char*) &val, sizeof(val));
		//add_code_change(address, val);
		core->rawWrite16(core, address, 8, val);

		address += 0x2;
	}

	mLog(_mLOG_CAT_GBA_DEBUG, mLogLevel::mLOG_DEBUG, "Path: %s", path);

	if (f.eof()) {
		mLog(_mLOG_CAT_GBA_DEBUG, mLogLevel::mLOG_DEBUG, "ROM loaded");
		add_code_change(MODLOADER_START + 0x2, 0x31DF); // swi #0x31 ; vm call to branch straight to the entry point
	} else {
		add_code_change(MODLOADER_START + 0x2, 0x001c); // [PLACEHOLDER] mov r0, r0
	}

	/* TryGenerateWildMon */
	add_code_change(0x08082B66, 0x30DF); // swi #0x30 ; custom vm call
	add_code_change(0x08082B68, 0x00BF); // nop

	mod_initialised = true;
}

void RedBirden_QueueWild(unsigned short species, unsigned char name[10]) {
	for (int i = 0; i < 9; i++) {
		if (name[i] == 0xFF) {
			break;
		}

		auto gamefreak_c = charmap.find(name[i]);
		if (gamefreak_c == charmap.end())
			name[i] = 0x0;
		else
			name[i] = gamefreak_c->second;
	}

	NextWild w;
	memcpy(w.name, name, 10);
	w.species = species;

	wild_queue.emplace(w);
}

unsigned short RedBirden_GetNextSpecies() {
	if (wild_queue.empty()) {
		return 0;
	} else {
		uint16_t customwild_species = wild_queue.front().species;

		/* The ID is used to specify if it is pending or not */
		priorityTask.taskId = taskId++;
		priorityTask.type = TaskType::CustomWild;
		memcpy(priorityTask.data, wild_queue.front().name, 10);

		wild_queue.pop();

		return customwild_species;
	}
}

void RedBirden_StartWalking() {
	state_update = 0;
}

void RedBirden_StartRiding() {
	state_update = 1;
}

void RedBirden_StatusAll(unsigned char flags) {
	Task t;
	t.type = StatusAll;
	t.data[0] = flags;
	task_queue.emplace(t);
}

unsigned int RedBirden_GetEntryPoint() {
	return mod_entrypoint;
}