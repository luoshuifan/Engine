// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "StateTreeState.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreeEditorTypes.h"
#include "Debugger/StateTreeDebuggerTypes.h"
#include "StateTreeEditorData.generated.h"

struct FStateTreeBindableStructDesc;
struct FStateTreeEditorPropertyPath;

class UStateTreeSchema;

namespace UE::StateTree::Editor
{
	// Name used to describe container of global items (other items use the path to the container State).  
	extern STATETREEEDITORMODULE_API const FString GlobalStateName;

	// Name used to describe container of property functions.
	extern STATETREEEDITORMODULE_API const FString PropertyFunctionStateName;
}


USTRUCT()
struct FStateTreeEditorBreakpoint
{
	GENERATED_BODY()

	FStateTreeEditorBreakpoint() = default;
	explicit FStateTreeEditorBreakpoint(const FGuid& ID, const EStateTreeBreakpointType BreakpointType)
		: ID(ID)
		, BreakpointType(BreakpointType)
	{
	}

	/** Unique Id of the Node or State associated to the breakpoint. */
	UPROPERTY()
	FGuid ID;

	/** The event type that should trigger the breakpoint (e.g. OnEnter, OnExit, etc.). */
	UPROPERTY()
	EStateTreeBreakpointType BreakpointType = EStateTreeBreakpointType::Unset;
};

UENUM()
enum class EStateTreeVisitor : uint8
{
	Continue,
	Break,
};

/**
 * Edit time data for StateTree asset. This data gets baked into runtime format before being used by the StateTreeInstance.
 */
UCLASS(BlueprintType, EditInlineNew, CollapseCategories)
class STATETREEEDITORMODULE_API UStateTreeEditorData : public UObject, public IStateTreeEditorPropertyBindingsOwner
{
	GENERATED_BODY()
	
public:
	UStateTreeEditorData();

	virtual void PostInitProperties() override;
	
	// IStateTreeEditorPropertyBindingsOwner
	virtual void GetAccessibleStructs(const FGuid TargetStructID, TArray<FStateTreeBindableStructDesc>& OutStructDescs) const override;
	virtual bool GetStructByID(const FGuid StructID, FStateTreeBindableStructDesc& OutStructDesc) const override;
	virtual bool GetDataViewByID(const FGuid StructID, FStateTreeDataView& OutDataView) const override;
	virtual FStateTreeEditorPropertyBindings* GetPropertyEditorBindings() override { return &EditorBindings; }
	virtual const FStateTreeEditorPropertyBindings* GetPropertyEditorBindings() const override { return &EditorBindings; }
	virtual FStateTreeBindableStructDesc FindContextData(const UStruct* ObjectType, const FString ObjectNameHint) const override;

	virtual EStateTreeVisitor EnumerateBindablePropertyFunctionNodes(TFunctionRef<EStateTreeVisitor(const UScriptStruct* NodeStruct, const FStateTreeBindableStructDesc& Desc, const FStateTreeDataView Value)> InFunc) const;

	virtual bool CanCreateParameter(const FGuid StructID) const override;
	virtual void CreateParameters(const FGuid StructID, TArrayView<FStateTreeEditorPropertyCreationDesc> InOutCreationDescs) override;
	// ~IStateTreeEditorPropertyBindingsOwner

	/**
	 * Returns the description for the node for UI.
	 * Handles the name override logic, figures out required data for the GetDescription() call, and handles the fallbacks.
	 * @return description for the node.
	 */
	FText GetNodeDescription(const FStateTreeEditorNode& Node, const EStateTreeNodeFormatting Formatting = EStateTreeNodeFormatting::Text) const;

#if WITH_EDITOR
	using FReplacementObjectMap = TMap<UObject*, UObject*>;
	void OnObjectsReinstanced(const FReplacementObjectMap& ObjectMap);
	void OnUserDefinedStructReinstanced(const UUserDefinedStruct& UserDefinedStruct);
	void OnParametersChanged(const UStateTree& StateTree);
	void OnStateParametersChanged(const UStateTree& StateTree, const FGuid StateID);
	virtual void BeginDestroy() override;
	virtual void PostLoad() override;
	virtual void PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent) override;
#endif

	/** @returns parent state of a struct, or nullptr if not found. */
	const UStateTreeState* GetStateByStructID(const FGuid TargetStructID) const;

	/** @returns state based on its ID, or nullptr if not found. */
	const UStateTreeState* GetStateByID(const FGuid StateID) const;

	/** @returns mutable state based on its ID, or nullptr if not found. */
	UStateTreeState* GetMutableStateByID(const FGuid StateID);

	/** @returns the IDs and instance values of all bindable structs in the StateTree. */
	void GetAllStructValues(TMap<FGuid, const FStateTreeDataView>& AllValues) const;

	/**
	* Iterates over all structs that are related to binding
	* @param InFunc function called at each node, should return Continue if visiting is continued or Break to stop.
	*/
	EStateTreeVisitor VisitHierarchy(TFunctionRef<EStateTreeVisitor(UStateTreeState& State, UStateTreeState* ParentState)> InFunc) const;

	/**
	 * Iterates over all structs at the global level (context, tree parameters, evaluators, global tasks) that are related to binding.
	 * @param InFunc function called at each node, should return Continue if visiting is continued or Break to stop.
	 */
	EStateTreeVisitor VisitGlobalNodes(TFunctionRef<EStateTreeVisitor(const UStateTreeState* State, const FStateTreeBindableStructDesc& Desc, const FStateTreeDataView Value)> InFunc) const;

	/**
	 * Iterates over all structs in the state hierarchy that are related to binding.
	 * @param InFunc function called at each node, should return Continue if visiting is continued or Break to stop.
	 */
	EStateTreeVisitor VisitHierarchyNodes(TFunctionRef<EStateTreeVisitor(const UStateTreeState* State, const FStateTreeBindableStructDesc& Desc, const FStateTreeDataView Value)> InFunc) const;

	/**
	 * Iterates over all structs that are related to binding.
	 * @param InFunc function called at each node, should return Continue if visiting is continued or Break to stop.
	 */
	EStateTreeVisitor VisitAllNodes(TFunctionRef<EStateTreeVisitor(const UStateTreeState* State, const FStateTreeBindableStructDesc& Desc, const FStateTreeDataView Value)> InFunc) const;

	/**
	 * Iterates over all nodes in a given state.
	 * @param InFunc function called at each node, should return Continue if visiting is continued or Break to stop.
	 */
	EStateTreeVisitor VisitStateNodes(const UStateTreeState& State, TFunctionRef<EStateTreeVisitor(const UStateTreeState* State, const FStateTreeBindableStructDesc& Desc, const FStateTreeDataView Value)> InFunc) const;

	/**
	 * Iterates recursively over all property functions of the provided node. Also nested ones.
	 * @param InFunc function called at each node, should return Continue if visiting is continued or Break to stop.
	 */
	EStateTreeVisitor VisitStructBoundPropertyFunctions(FGuid StructID, const FString& StatePath, TFunctionRef<EStateTreeVisitor(const FStateTreeEditorNode& EditorNode, const FStateTreeBindableStructDesc& Desc, const FStateTreeDataView Value)> InFunc) const;

	/**
	 * Returns array of nodes along the execution path, up to the TargetStruct.
	 * @param Path The states to visit during the check
	 * @param TargetStructID The ID of the node where to stop.
	 * @param OutStructDescs Array of nodes accessible on the given path.  
	 */
	void GetAccessibleStructs(const TConstArrayView<const UStateTreeState*> Path, const FGuid TargetStructID, TArray<FStateTreeBindableStructDesc>& OutStructDescs) const;

	UE_DEPRECATED(5.3, "Use VisitHierarchyNodes with State, Desc, Value instead.")
	void VisitHierarchyNodes(TFunctionRef<EStateTreeVisitor(const UStateTreeState* State, const FGuid& ID, const FName& Name, const EStateTreeNodeType NodeType, const UScriptStruct* NodeStruct, const UStruct* InstanceStruct)> InFunc) const;

	UE_DEPRECATED(5.3, "Use VisitStateNodes with State, Desc, Value instead.")
	EStateTreeVisitor VisitStateNodes(const UStateTreeState& State, TFunctionRef<EStateTreeVisitor(const UStateTreeState* State, const FGuid& ID, const FName& Name, const EStateTreeNodeType NodeType, const UScriptStruct* NodeStruct, const UStruct* InstanceStruct)> InFunc) const;

	UE_DEPRECATED(5.3, "Use GetAllStructValues with values instead.")
	void GetAllStructIDs(TMap<FGuid, const UStruct*>& AllStructs) const;

	void ReparentStates();
	
	// StateTree Builder API

	/**
	 * Adds new Subtree with specified name.
	 * @return Pointer to the new Subtree.
	 */
	UStateTreeState& AddSubTree(const FName Name)
	{
		UStateTreeState* SubTreeState = NewObject<UStateTreeState>(this, FName(), RF_Transactional);
		check(SubTreeState);
		SubTreeState->Name = Name;
		SubTrees.Add(SubTreeState);
		return *SubTreeState;
	}

	/**
	 * Adds new Subtree named "Root".
	 * @return Pointer to the new Subtree.
	 */
	UStateTreeState& AddRootState()
	{
		return AddSubTree(FName(TEXT("Root")));
	}

	/**
	 * Adds Evaluator of specified type.
	 * @return reference to the new Evaluator. 
	 */
	template<typename T, typename... TArgs>
	TStateTreeEditorNode<T>& AddEvaluator(TArgs&&... InArgs)
	{
		FStateTreeEditorNode& EditorNode = Evaluators.AddDefaulted_GetRef();
		EditorNode.ID = FGuid::NewGuid();
		EditorNode.Node.InitializeAs<T>(Forward<TArgs>(InArgs)...);
		T& Eval = EditorNode.Node.GetMutable<T>();
		if (const UScriptStruct* InstanceType = Cast<const UScriptStruct>(Eval.GetInstanceDataType()))
		{
			EditorNode.Instance.InitializeAs(InstanceType);
		}
		return static_cast<TStateTreeEditorNode<T>&>(EditorNode);
	}

	/**
	 * Adds Global Task of specified type.
	 * @return reference to the new task. 
	 */
	template<typename T, typename... TArgs>
	TStateTreeEditorNode<T>& AddGlobalTask(TArgs&&... InArgs)
	{
		FStateTreeEditorNode& EditorNode = GlobalTasks.AddDefaulted_GetRef();
		EditorNode.ID = FGuid::NewGuid();
		EditorNode.Node.InitializeAs<T>(Forward<TArgs>(InArgs)...);
		T& Task = EditorNode.Node.GetMutable<T>();
		if (const UScriptStruct* InstanceType = Cast<const UScriptStruct>(Task.GetInstanceDataType()))
		{
			EditorNode.Instance.InitializeAs(InstanceType);
		}
		return static_cast<TStateTreeEditorNode<T>&>(EditorNode);
	}

	/**
	 * Adds property binding between two structs.
	 */
	void AddPropertyBinding(const FStateTreePropertyPath& SourcePath, const FStateTreePropertyPath& TargetPath)
	{
		EditorBindings.AddPropertyBinding(SourcePath, TargetPath);
	}

	/**
	 * Adds property binding to PropertyFunction of provided type.
	 */
	void AddPropertyBinding(const UScriptStruct* PropertyFunctionNodeStruct, TConstArrayView<FStateTreePropertyPathSegment> SourcePathSegments, const FStateTreePropertyPath& TargetPath)
	{
		EditorBindings.AddFunctionPropertyBinding(PropertyFunctionNodeStruct, SourcePathSegments, TargetPath);
	}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
	UE_DEPRECATED(5.3, "Use version with FStateTreePropertyPath instead.")
	void AddPropertyBinding(const FStateTreeEditorPropertyPath& SourcePath, const FStateTreeEditorPropertyPath& TargetPath);
PRAGMA_ENABLE_DEPRECATION_WARNINGS

	/**
	 * Adds property binding between two structs.
	 */
	bool AddPropertyBinding(const FStateTreeEditorNode& SourceNode, const FString SourcePathStr, const FStateTreeEditorNode& TargetNode, const FString TargetPathStr)
	{
		FStateTreePropertyPath SourcePath;
		FStateTreePropertyPath TargetPath;
		SourcePath.SetStructID(SourceNode.ID);
		TargetPath.SetStructID(TargetNode.ID);
		if (SourcePath.FromString(SourcePathStr) && TargetPath.FromString(TargetPathStr))
		{
			EditorBindings.AddPropertyBinding(SourcePath, TargetPath);
			return true;
		}
		return false;
	}

#if WITH_STATETREE_TRACE_DEBUGGER
	bool HasAnyBreakpoint(FGuid ID) const;
	bool HasBreakpoint(FGuid ID, EStateTreeBreakpointType BreakpointType) const;
	const FStateTreeEditorBreakpoint* GetBreakpoint(FGuid ID, EStateTreeBreakpointType BreakpointType) const;
	void AddBreakpoint(FGuid ID, EStateTreeBreakpointType BreakpointType);
	bool RemoveBreakpoint(FGuid ID, EStateTreeBreakpointType BreakpointType);
#endif // WITH_STATETREE_TRACE_DEBUGGER

	// ~StateTree Builder API

	/**
	 * Attempts to find a Color matching the provided Color Key
	 */
	const FStateTreeEditorColor* FindColor(const FStateTreeEditorColorRef& ColorRef) const
	{
		return Colors.Find(FStateTreeEditorColor(ColorRef));
	}

private:
	void FixObjectInstance(TSet<UObject*>& SeenObjects, UObject& Outer, FStateTreeEditorNode& Node);
	void FixObjectNodes();
	void FixDuplicateIDs();
	void UpdateBindingsInstanceStructs();
	void CallPostLoadOnNodes();

#if WITH_EDITORONLY_DATA
	FDelegateHandle OnObjectsReinstancedHandle;
	FDelegateHandle OnUserDefinedStructReinstancedHandle;
	FDelegateHandle OnParametersChangedHandle;
	FDelegateHandle OnStateParametersChangedHandle;
#endif

public:
	/** Schema describing which inputs, evaluators, and tasks a StateTree can contain */	
	UPROPERTY(EditDefaultsOnly, Category = Common, Instanced)
	TObjectPtr<UStateTreeSchema> Schema = nullptr;

	/** Public parameters that could be used for bindings within the Tree. */
	UPROPERTY(EditDefaultsOnly, Category = Parameters)
	FStateTreeStateParameters RootParameters;

	UPROPERTY(EditDefaultsOnly, Category = "Evaluators", meta = (BaseStruct = "/Script/StateTreeModule.StateTreeEvaluatorBase", BaseClass = "/Script/StateTreeModule.StateTreeEvaluatorBlueprintBase"))
	TArray<FStateTreeEditorNode> Evaluators;

	UPROPERTY(EditDefaultsOnly, Category = "Global Tasks", meta = (BaseStruct = "/Script/StateTreeModule.StateTreeTaskBase", BaseClass = "/Script/StateTreeModule.StateTreeTaskBlueprintBase"))
	TArray<FStateTreeEditorNode> GlobalTasks;

	UPROPERTY(meta = (ExcludeFromHash))
	FStateTreeEditorPropertyBindings EditorBindings;

	/** Color Options to assign to a State */
	UPROPERTY(EditDefaultsOnly, Category = "Theme")
	TSet<FStateTreeEditorColor> Colors;

	/** Top level States. */
	UPROPERTY()
	TArray<TObjectPtr<UStateTreeState>> SubTrees;

	/**
	 * Transient list of breakpoints added in the debugging session.
	 * These will be lost if the asset gets reloaded.
	 * If there is eventually a change to make those persist with the asset
	 * we need to prune all dangling breakpoints after states/tasks got removed. (see RemoveUnusedBindings)
	 */
	UPROPERTY(Transient)
	TArray<FStateTreeEditorBreakpoint> Breakpoints;
};
