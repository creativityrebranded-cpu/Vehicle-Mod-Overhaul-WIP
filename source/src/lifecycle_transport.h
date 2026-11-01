#pragma once
#include "lifecycle_requests.h"
#include <Windows.h>
#include <cstddef>
#include <new>

namespace lifecycle {
// Pinned BSService arena ABI, 1.16.244: 1418C2720 / 1418CE340,
// 1422CDF60 / 1422CE200. Arena callbacks destroy captures; pages own storage.
struct Reservation { std::uintptr_t page; unsigned offset; int size; };
static_assert(sizeof(Reservation)==16);
struct Command {
    const void* vtable;
    void (*callback)(Command*);
    Ticket ticket;
    static void* Destroy(Command* command,unsigned) { command->ticket.~shared_ptr();return command; }
    static void Execute(Command* command) { command->ticket->Run(); }
    static bool False(Command*) { return false; }
    static void Invoke(Command* command) { Execute(command);Destroy(command,0); }
};
static_assert(offsetof(Command,callback)==8 && offsetof(Command,ticket)==0x10);
inline const void* const kCommandVtable[]={reinterpret_cast<const void*>(&Command::Destroy),reinterpret_cast<const void*>(&Command::Execute),reinterpret_cast<const void*>(&Command::False)};
using Allocate=void* (*)(std::uintptr_t,Reservation*,int,unsigned);
inline bool Enqueue(std::uintptr_t queue,Allocate allocate,const Ticket& ticket) {
    if(!queue||!allocate)return false;
    Reservation slot{};
    void* memory=allocate(queue,&slot,sizeof(Command),alignof(Command));
    // Native allocator reserves the command before returning. It has no
    // fallible publication path: construct only noexcept fields here.
    if(!memory)return false;
    new(memory) Command{kCommandVtable,&Command::Invoke,ticket};
    const auto old=static_cast<unsigned>(InterlockedExchange(reinterpret_cast<volatile LONG*>(slot.page+0x10),slot.offset));
    const unsigned capacity=*reinterpret_cast<const unsigned*>(queue+0x164);
    if(old<capacity)*reinterpret_cast<unsigned*>(slot.page+old+0x44)=slot.offset;
    else *reinterpret_cast<unsigned*>(slot.page+0x14)=slot.offset;
    InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(slot.page+0xC),slot.size);
    return true;
}
}
