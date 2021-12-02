#pragma once

struct mCore;

extern unsigned char current_name[10];
extern unsigned short customwild_species;

void RedBirden_RunMods(struct mCore* core);
void RedBirden_InitMods(struct mCore* core);
void RedBirden_QueueWild(unsigned short species, unsigned char name[10]);
unsigned short RedBirden_GetNextSpecies();
unsigned int RedBirden_GetEntryPoint();
void RedBirden_StartWalking();
void RedBirden_StartRiding();
void RedBirden_StatusAll(unsigned char flags);