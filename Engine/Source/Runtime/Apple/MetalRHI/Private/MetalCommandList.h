// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once

#include <Metal/Metal.h>

class FMetalCommandQueue;

/**
 * FMetalCommandList:
 * Encapsulates multiple command-buffers into an ordered list for submission. 
 * For the immediate context this is irrelevant and is merely a pass-through into the CommandQueue, but
 * for deferred/parallel contexts it is required as they must queue their command buffers until they can 
 * be committed to the command-queue in the proper order which is only known at the end of parallel encoding.
 */
class FMetalCommandList
{
public:
#pragma mark - Public C++ Boilerplate -

	/**
	 * Constructor
	 * @param InCommandQueue The command-queue to which the command-list's buffers will be submitted.
	 */
	FMetalCommandList(FMetalCommandQueue& InCommandQueue, bool const bInImmediate);
	
	/** Destructor */
	~FMetalCommandList(void);
	
#pragma mark - Public Command List Mutators -
	
	/** 
	 * Commits the provided buffer to the command-list for execution. When parallel encoding this will be submitted later.
	 * @param Buffer The buffer to submit to the command-list.
	 * @param bWait Whether to wait for the command buffer to complete - it is an error to set this to true on a deferred command-list.
	 */
	void Commit(id<MTLCommandBuffer> Buffer, bool const bWait);
	
	/**
	 * Submits all outstanding command-buffers in the proper commit order to the command-queue.
	 * When more than one command-list is active the command-queue will buffer the command-lists until all are committed to guarantee order of submission to the GPU.
	 * @param Index The command-list's intended index in the command-queue.
	 * @param Count The number of command-lists that will be committed to the command-queue.
	 */
	void Submit(uint32 Index, uint32 Count);
	
	/** Called by the command-queue once all the buffers have been scheduled for execution. */
	void OnScheduled(void);
	
#pragma mark - Public Command List Accessors -

	/**
	* Accessor to the internal array of submitted command buffers waiting for execution - called by the Command Queue.
	* @returns The internal array of submitted command buffers waiting for scheduling.
	*/
	NSArray<id<MTLCommandBuffer>>* GetCommandBuffers() const;
	
private:
#pragma mark - Private Member Variables -
	FMetalCommandQueue& CommandQueue;
	NSMutableArray<id<MTLCommandBuffer>>* SubmittedBuffers;
	bool bImmediate;
};