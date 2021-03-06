// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

// Structs used for passing parameters to scene query functions

#pragma once

#include "Engine/EngineTypes.h"

/** Macro to convert ECollisionChannels to bit flag **/
#define ECC_TO_BITFIELD(x)	(1<<(x))
/** Macro to convert from CollisionResponseContainer to bit flag **/
#define CRC_TO_BITFIELD(x)	(1<<(x))

/** Structure that defines parameters passed into collision function */
struct ENGINE_API FCollisionQueryParams
{
	/** Tag used to provide extra information or filtering for debugging of the trace (e.g. Collision Analyzer) */
	FName TraceTag;

	/** Tag used to indicate an owner for this trace */
	FName OwnerTag;

	/** Whether we should perform the trace in the asynchronous scene.  Default is false. */
	bool bTraceAsyncScene;

	/** Whether we should trace against complex collision */
	bool bTraceComplex;

	/** Whether we want to find out initial overlap or not. If true, it will return if this was initial overlap. */
	bool bFindInitialOverlaps;

	/** Whether we want to return the triangle face index for complex static mesh traces */
	bool bReturnFaceIndex;

	/** Only fill in the PhysMaterial field of  */
	bool bReturnPhysicalMaterial;

	/** Whether to ignore blocking results. */
	bool bIgnoreBlocks;

	/** TArray typedef of components to ignore. */
	typedef TArray<uint32, TInlineAllocator<NumInlinedActorComponents>> IgnoreComponentsArrayType;

	/** Extra filtering done on the query. See declaration for filtering logic */
	FMaskFilter IgnoreMask;

private:

	/** Tracks whether the IgnoreComponents list is verified unique. */
	mutable bool bComponentListUnique;

	/** Set of components to ignore during the trace */
	mutable IgnoreComponentsArrayType IgnoreComponents;

	void Internal_AddIgnoredComponent(const UPrimitiveComponent* InIgnoreComponent);

public:

	/** Returns set of unique components to ignore during the trace. Elements are guaranteed to be unique (they are made so internally if they are not already). */
	const IgnoreComponentsArrayType& GetIgnoredComponents() const;

	/** Clears the set of components to ignore during the trace. */
	void ClearIgnoredComponents()
	{
		IgnoreComponents.Reset();
		bComponentListUnique = true;
	}

	/**
	 * Set the number of ignored components in the list. Uniqueness is not changed, it operates on the current state (unique or not).
	 * Useful for temporarily adding some, then restoring to a previous size. NewNum must be <= number of current components for there to be any effect.
	 */
	void SetNumIgnoredComponents(int32 NewNum);

	// Constructors

	/** 
	 *  DEPRECATED!  
	 *  Please instead provide a FName parameter when constructing a FCollisionQueryParams object which will use the other constructor.
	 *  Providing a single string literal argument, such as TEXT("foo"), instead of an explicit FNAME
	 *  can cause this constructor to be invoked instead the the other which was likely the programmers intention. 
	 *  This constructor will eventually be deprecated to avoid this potentially ambiguous case.
	 */ 
	DEPRECATED(4.11, "FCollisionQueryParams, to avoid ambiguity, please use other constructor and explicitly provide an FName parameter (not just a string literal) as the first parameter")
	FCollisionQueryParams(bool bInTraceComplex)
	{
		bTraceComplex = bInTraceComplex;
		TraceTag = NAME_None;
		bTraceAsyncScene = false;
		bFindInitialOverlaps = true;
		bReturnFaceIndex = false;
		bReturnPhysicalMaterial = false;
		bComponentListUnique = true;
		IgnoreMask = 0;
		bIgnoreBlocks = false;
	}

	FCollisionQueryParams()
	{
		bTraceComplex = false;
		TraceTag = NAME_None;
		bTraceAsyncScene = false;
		bFindInitialOverlaps = true;
		bReturnFaceIndex = false;
		bReturnPhysicalMaterial = false;
		bComponentListUnique = true;
		IgnoreMask = 0;
		bIgnoreBlocks = false;
	}



	FCollisionQueryParams(FName InTraceTag, bool bInTraceComplex=false, const AActor* InIgnoreActor=NULL);

	// Utils

	/** Add an actor for this trace to ignore */
	void AddIgnoredActor(const AActor* InIgnoreActor);

	/** Add a collection of actors for this trace to ignore */
	void AddIgnoredActors(const TArray<AActor*>& InIgnoreActors);

	/** Variant that uses an array of TWeakObjectPtrs */
	void AddIgnoredActors(const TArray<TWeakObjectPtr<AActor> >& InIgnoreActors);

	/** Add a component for this trace to ignore */
	void AddIgnoredComponent(const UPrimitiveComponent* InIgnoreComponent);

	/** Add a collection of components for this trace to ignore */
	void AddIgnoredComponents(const TArray<UPrimitiveComponent*>& InIgnoreComponents);
	
	/** Variant that uses an array of TWeakObjectPtrs */
	void AddIgnoredComponents(const TArray<TWeakObjectPtr<UPrimitiveComponent>>& InIgnoreComponents);

	/**
	 * Special variant that hints that we are likely adding a duplicate of the root component or first ignored component.
	 * Helps avoid invalidating the potential uniquess of the IgnoreComponents array.
	 */
	void AddIgnoredComponent_LikelyDuplicatedRoot(const UPrimitiveComponent* InIgnoreComponent);

	FString ToString() const
	{
		return FString::Printf(TEXT("[%s:%s] TraceAsync(%d), TraceComplex(%d)"), *OwnerTag.ToString(), *TraceTag.ToString(), bTraceAsyncScene, bTraceComplex );
	}

	/** static variable for default data to be used without reconstructing everytime **/
	static FCollisionQueryParams DefaultQueryParam;
};

/** Structure when performing a collision query using a component's geometry */
struct ENGINE_API FComponentQueryParams : public FCollisionQueryParams
{
	FComponentQueryParams() 
	: FCollisionQueryParams(NAME_None,false)
	{
	}

	FComponentQueryParams(FName InTraceTag, const AActor* InIgnoreActor=NULL)
	: FCollisionQueryParams(InTraceTag, false, InIgnoreActor)
	{
	}

	/** static variable for default data to be used without reconstructing everytime **/
	static FComponentQueryParams DefaultComponentQueryParams;
};

/** Structure that defines response container for the query. Advanced option. */
struct ENGINE_API FCollisionResponseParams
{
	/** 
	 *	Collision Response container for trace filtering. If you'd like to ignore certain channel for this trace, use this struct.
	 *	By default, every channel will be blocked
	 */
	struct FCollisionResponseContainer CollisionResponse;

	FCollisionResponseParams(ECollisionResponse DefaultResponse = ECR_Block)
	{
		CollisionResponse.SetAllChannels(DefaultResponse);
	}

	FCollisionResponseParams(const FCollisionResponseContainer& ResponseContainer)
	{
		CollisionResponse = ResponseContainer;
	}
	/** static variable for default data to be used without reconstructing everytime **/
	static FCollisionResponseParams DefaultResponseParam;
};

// If ECollisionChannel entry has metadata of "TraceType = 1", they will be excluded by Collision Profile
// Any custom channel with bTraceType=true also will be excluded
// By default everything is object type
struct ENGINE_API FCollisionQueryFlag
{
private:
	int32 AllObjectQueryFlag;
	int32 AllStaticObjectQueryFlag;

	FCollisionQueryFlag()
	{
		AllObjectQueryFlag = 0xFFFFFFFF;
		AllStaticObjectQueryFlag = ECC_TO_BITFIELD(ECC_WorldStatic);
	}

public:
	static struct FCollisionQueryFlag & Get()
	{
		static FCollisionQueryFlag CollisionQueryFlag;
		return CollisionQueryFlag;
	}

	int32 GetAllObjectsQueryFlag()
	{
		// this doesn't really verify trace queries to come this way
		return AllObjectQueryFlag;
	}

	int32 GetAllStaticObjectsQueryFlag()
	{
		return AllStaticObjectQueryFlag;
	}

	int32 GetAllDynamicObjectsQueryFlag()
	{
		return (AllObjectQueryFlag & ~AllStaticObjectQueryFlag);
	}

	void AddToAllObjectsQueryFlag(ECollisionChannel NewChannel)
	{
		if (ensure ((int32)NewChannel < 32))
		{
			SetAllObjectsQueryFlag (AllObjectQueryFlag |= ECC_TO_BITFIELD(NewChannel));
		}
	}

	void AddToAllStaticObjectsQueryFlag(ECollisionChannel NewChannel)
	{
		if (ensure ((int32)NewChannel < 32))
		{
			SetAllStaticObjectsQueryFlag (AllStaticObjectQueryFlag |= ECC_TO_BITFIELD(NewChannel));
		}
	}

	void RemoveFromAllObjectsQueryFlag(ECollisionChannel NewChannel)
	{
		if (ensure ((int32)NewChannel < 32))
		{
			SetAllObjectsQueryFlag (AllObjectQueryFlag &= ~ECC_TO_BITFIELD(NewChannel));
		}
	}

	void RemoveFromAllStaticObjectsQueryFlag(ECollisionChannel NewChannel)
	{
		if (ensure ((int32)NewChannel < 32))
		{
			SetAllStaticObjectsQueryFlag (AllObjectQueryFlag &= ~ECC_TO_BITFIELD(NewChannel));
		}
	}

	void SetAllObjectsQueryFlag(int32 NewQueryFlag)
	{
		// if all object query has changed, make sure to apply to static object query
		AllObjectQueryFlag = NewQueryFlag;
		AllStaticObjectQueryFlag = AllObjectQueryFlag & AllStaticObjectQueryFlag;
	}

	void SetAllStaticObjectsQueryFlag(int32 NewQueryFlag)
	{
		AllStaticObjectQueryFlag = NewQueryFlag;
	}

	void SetAllDynamicObjectsQueryFlag(int32 NewQueryFlag)
	{
		AllStaticObjectQueryFlag = AllObjectQueryFlag & ~NewQueryFlag;
	}
};

/** Structure that contains list of object types the query is intersted in.  */
struct ENGINE_API FCollisionObjectQueryParams
{
	enum InitType
	{
		AllObjects,
		AllStaticObjects,
		AllDynamicObjects
	};

	/** Set of object type queries that it is interested in **/
	int32 ObjectTypesToQuery;

	/** Extra filtering done during object query. See declaration for filtering logic */
	FMaskFilter IgnoreMask;

	FCollisionObjectQueryParams()
		: ObjectTypesToQuery(0)
		, IgnoreMask(0)
	{
	}

	FCollisionObjectQueryParams(ECollisionChannel QueryChannel)
	{
		ObjectTypesToQuery = ECC_TO_BITFIELD(QueryChannel);
		IgnoreMask = 0;
	}

	FCollisionObjectQueryParams(const TArray<TEnumAsByte<EObjectTypeQuery> > & ObjectTypes)
	{
		ObjectTypesToQuery = 0;

		for (auto Iter = ObjectTypes.CreateConstIterator(); Iter; ++Iter)
		{
			AddObjectTypesToQuery(UEngineTypes::ConvertToCollisionChannel((*Iter).GetValue()));
		}

		IgnoreMask = 0;
	}

	FCollisionObjectQueryParams(enum FCollisionObjectQueryParams::InitType QueryType)
	{
		switch (QueryType)
		{
		case AllObjects:
			ObjectTypesToQuery = FCollisionQueryFlag::Get().GetAllObjectsQueryFlag();
			break;
		case AllStaticObjects:
			ObjectTypesToQuery = FCollisionQueryFlag::Get().GetAllStaticObjectsQueryFlag();
			break;
		case AllDynamicObjects:
			ObjectTypesToQuery = FCollisionQueryFlag::Get().GetAllDynamicObjectsQueryFlag();
			break;
		}

		IgnoreMask = 0;
		
	};

	// to do this, use ECC_TO_BITFIELD to convert to bit field
	// i.e. FCollisionObjectQueryParams ( ECC_TO_BITFIELD(ECC_WorldStatic) | ECC_TO_BITFIELD(ECC_WorldDynamic) )
	FCollisionObjectQueryParams(int32 InObjectTypesToQuery)
	{
		ObjectTypesToQuery = InObjectTypesToQuery;
		IgnoreMask = 0;
		DoVerify();
	}

	void AddObjectTypesToQuery(ECollisionChannel QueryChannel)
	{
		ObjectTypesToQuery |= ECC_TO_BITFIELD(QueryChannel);
		DoVerify();
	}

	void RemoveObjectTypesToQuery(ECollisionChannel QueryChannel)
	{
		ObjectTypesToQuery &= ~ECC_TO_BITFIELD(QueryChannel);
		DoVerify();
	}

	int32 GetQueryBitfield() const
	{
		checkSlow(IsValid());

		return ObjectTypesToQuery;
	}

	bool IsValid() const
	{ 
		return (ObjectTypesToQuery != 0); 
	}

	static bool IsValidObjectQuery(ECollisionChannel QueryChannel) 
	{
		// return true if this belong to object query type
		return (ECC_TO_BITFIELD(QueryChannel) & FCollisionQueryFlag::Get().GetAllObjectsQueryFlag()) != 0;
	}

	void DoVerify() const
	{
		// you shouldn't use Trace Types to be Object Type Query parameter. This is not technical limitation, but verification process. 
		checkSlow((ObjectTypesToQuery & FCollisionQueryFlag::Get().GetAllObjectsQueryFlag()) == ObjectTypesToQuery);
	}

	/** Internal. */
	static inline FCollisionObjectQueryParams::InitType GetCollisionChannelFromOverlapFilter(EOverlapFilterOption Filter)
	{
		static FCollisionObjectQueryParams::InitType ConvertMap[3] = { FCollisionObjectQueryParams::InitType::AllObjects, FCollisionObjectQueryParams::InitType::AllDynamicObjects, FCollisionObjectQueryParams::InitType::AllStaticObjects };
		return ConvertMap[Filter];
	}
	static FCollisionObjectQueryParams DefaultObjectQueryParam;
};

