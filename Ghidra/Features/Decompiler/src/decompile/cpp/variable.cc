/* ###
 * IP: GHIDRA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *      http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "variable.hh"
#include "op.hh"
#include "database.hh"

AttributeId ATTRIB_CLASS = AttributeId("class",66);
AttributeId ATTRIB_REPREF = AttributeId("repref",67);
AttributeId ATTRIB_SYMREF = AttributeId("symref",68);

ElementId ELEM_HIGH = ElementId("high",82);

/// Compare by offset within the group, then by size.
/// \param op2 is the other piece to compare with \b this
/// \return \b true if \b this should be ordered before the other piece
bool VariableGroup::PieceCompareByOffset::operator()(const VariablePiece *a,const VariablePiece *b) const

{
  if (a->getOffset() != b->getOffset())
    return (a->getOffset() < b->getOffset());
  return (a->getSize() < b->getSize());
}

/// The VariablePiece takes partial ownership of \b this, via refCount.
/// \param piece is the new piece to add
void VariableGroup::addPiece(VariablePiece *piece)

{
  piece->group = this;
  if (!pieceSet.insert(piece).second)
    throw LowlevelError("Duplicate VariablePiece");
}

void VariableGroup::removePiece(VariablePiece *piece)

{
  pieceSet.erase(piece);
}

/// Construct piece given a HighVariable and its position within the whole.
/// If \b this is the first piece in the group, allocate a new VariableGroup object.
/// \param h is the given HighVariable to treat as a piece
/// \param offset is the byte offset of the piece within the whole
/// \param grp is another HighVariable in the whole, or null if \b this is the first piece
VariablePiece::VariablePiece(HighVariable *h,int4 offset,HighVariable *grp)

{
  high = h;
  groupOffset = offset;
  size = h->getInstance(0)->getSize();
  if (grp != (HighVariable *)0)
    group = grp->piece->getGroup();
  else
    group = new VariableGroup();
  group->addPiece(this);
}

VariablePiece::~VariablePiece(void)

{
  group->removePiece(this);
  if (group->empty())
    delete group;
  else
    markIntersectionDirty();
}

void VariablePiece::markIntersectionDirty(void) const

{
  set<VariablePiece *,VariableGroup::PieceCompareByOffset>::const_iterator iter;

  for(iter=group->pieceSet.begin();iter!=group->pieceSet.end();++iter)
    (*iter)->high->highflags |= (HighVariable::intersectdirty | HighVariable::extendcoverdirty);
}

void VariablePiece::markExtendCoverDirty(void) const

{
  if ((high->highflags & HighVariable::intersectdirty)!=0)
    return;	// intersection list itself is dirty, extended covers will be recomputed anyway
  for(int4 i=0;i<intersection.size();++i) {
    intersection[i]->high->highflags |= HighVariable::extendcoverdirty;
  }
  high->highflags |= HighVariable::extendcoverdirty;
}

/// Compute list of exactly the HighVariable pieces that intersect with \b this.
void VariablePiece::updateIntersections(void) const

{
  if ((high->highflags & HighVariable::intersectdirty)==0) return;
  set<VariablePiece *,VariableGroup::PieceCompareByOffset>::const_iterator iter;

  int4 endOffset = groupOffset + size;
  intersection.clear();
  for(iter=group->pieceSet.begin();iter!=group->pieceSet.end();++iter) {
    VariablePiece *otherPiece = *iter;
    if (otherPiece == this) continue;
    if (endOffset <= otherPiece->groupOffset) continue;
    int4 otherEndOffset = otherPiece->groupOffset + otherPiece->size;
    if (groupOffset >= otherEndOffset) continue;
    intersection.push_back(otherPiece);
  }
  high->highflags &= ~(uint4)HighVariable::intersectdirty;
}

/// Union internal covers of all pieces intersecting with \b this.
void VariablePiece::updateCover(void) const

{
  if ((high->highflags & (HighVariable::coverdirty | HighVariable::extendcoverdirty))==0) return;
  high->updateInternalCover();
  cover = high->internalCover;
  for(int4 i=0;i<intersection.size();++i) {
    const HighVariable *high = intersection[i]->high;
    high->updateInternalCover();
    cover.merge(high->internalCover);
  }
  high->highflags &= ~(uint4)HighVariable::extendcoverdirty;
}

/// \param amt is the given amout to add to offset
void VariablePiece::adjustOffset(int4 amt)

{
  set<VariablePiece *,VariableGroup::PieceCompareByOffset>::iterator iter;

  for(iter=group->pieceSet.begin();iter!=group->pieceSet.end();++iter) {
    (*iter)->groupOffset += amt;
  }
}

/// If there are no remaining references to the old VariableGroup it is deleted.
/// \param newGropu is the new VariableGroup to transfer \b this to
void VariablePiece::transferGroup(VariableGroup *newGroup)

{
  group->removePiece(this);
  if (group->empty())
    delete group;
  newGroup->addPiece(this);
}

/// Combine the VariableGroup associated with the given other VariablePiece and the VariableGroup of \b this
/// into one group. Combining in this way requires pieces of the same size and offset to be merged. This
/// method does not do the merging but passes back a list of HighVariable pairs that need to be merged.
/// The first element in the pair will have its VariablePiece in the new group, and the second element
/// will have its VariablePiece freed in preparation for the merge.
/// Offsets are adjusted so that \b this and the given other piece have the same offset;
/// \param op2 is the given other VariablePiece
/// \param mergePairs passes back the collection of HighVariable pairs that must be merged
void VariablePiece::combineOtherGroup(VariablePiece *op2,vector<HighVariable *> &mergePairs)

{
  int4 diff = groupOffset - op2->groupOffset;	// Add to op2, or subtract from this
  if (diff > 0)
    op2->adjustOffset(diff);
  else if (diff < 0)
    adjustOffset(-diff);
  set<VariablePiece *,VariableGroup::PieceCompareByOffset>::iterator iter = op2->group->pieceSet.begin();
  set<VariablePiece *,VariableGroup::PieceCompareByOffset>::iterator enditer = op2->group->pieceSet.end();
  while(iter != enditer) {
    VariablePiece *piece = *iter;
    set<VariablePiece *,VariableGroup::PieceCompareByOffset>::iterator matchiter = group->pieceSet.find(piece);
    if (matchiter != group->pieceSet.end()) {
      mergePairs.push_back((*matchiter)->high);
      mergePairs.push_back(piece->high);
      piece->high->piece = (VariablePiece *)0;	// Detach HighVariable from its original VariablePiece
      delete piece;
    }
    else
      piece->transferGroup(group);
  }
}

/// The new instance starts off with no associate Symbol and all properties marked as \e dirty.
/// \param vn is the single Varnode member
HighVariable::HighVariable(Varnode *vn)

{
  numMergeClasses = 1;
  highflags = flagsdirty | namerepdirty | typedirty | coverdirty;
  flags = 0;
  type = (Datatype *)0;
  piece = (VariablePiece *)0;
  symbol = (Symbol *)0;
  nameRepresentative = (Varnode *)0;
  symboloffset = -1;
  inst.push_back(vn);
  vn->setHigh( this, numMergeClasses-1 );
  if (vn->getSymbolEntry() != (SymbolEntry *)0)
    setSymbol(vn);
}

HighVariable::~HighVariable(void)

{
  if (piece != (VariablePiece *)0)
    delete piece;
}

/// The given Varnode \b must be a member and \b must have a non-null SymbolEntry
void HighVariable::setSymbol(Varnode *vn) const

{
  SymbolEntry *entry = vn->getSymbolEntry();
  if (symbol != (Symbol *)0 && symbol != entry->getSymbol()) {
    if ((highflags & symboldirty)==0) {
      ostringstream s;
      s << "Symbols \"" << symbol->getName() << "\" and \"" << entry->getSymbol()->getName();
      s << "\" assigned to the same variable";
      throw LowlevelError(s.str());
    }
  }
  symbol = entry->getSymbol();
  if (entry->isDynamic())	// Dynamic symbols match whole variable
    symboloffset = -1;
  else if (symbol->getCategory() == Symbol::equate)
    symboloffset = -1;			// For equates, we don't care about size
  else if (symbol->getType()->getSize() == vn->getSize() &&
      entry->getAddr() == vn->getAddr() && !entry->isPiece())
    symboloffset = -1;			// A matching entry
  else
    symboloffset = vn->getAddr().overlap(0,entry->getAddr(),symbol->getType()->getSize()) + entry->getOffset();

  highflags &= ~((uint4)symboldirty);		// We are no longer dirty
}

/// Link information to \b this from a Symbol that is not attached to a member Varnode.
/// This only works for a HighVariable with a constant member Varnode.  This used when there
/// is a constant address reference to the Symbol and the Varnode holds the reference, not
/// the actual value of the Symbol.
/// \param sym is the given Symbol to attach
/// \param off is the byte offset into the Symbol of the reference
void HighVariable::setSymbolReference(Symbol *sym,int4 off)

{
  symbol = sym;
  symboloffset = off;
  highflags &= ~((uint4)symboldirty);
}

void HighVariable::transferPiece(HighVariable *tv2)

{
  piece = tv2->piece;
  tv2->piece = (VariablePiece *)0;
  piece->setHigh(this);
  highflags |= (tv2->highflags & (intersectdirty | extendcoverdirty));
  tv2->highflags &= ~(uint4)(intersectdirty | extendcoverdirty);
}

/// Only update if the cover is marked as \e dirty.
/// Merge the covers of all Varnode instances.
void HighVariable::updateInternalCover(void) const

{
  if ((highflags & coverdirty) != 0) {
    internalCover.clear();
    if (inst[0]->hasCover()) {
      for(int4 i = 0;i < inst.size();++i)
	internalCover.merge(*inst[i]->getCover());
    }
    highflags &= ~coverdirty;
  }
}

/// This is \b only called by the Merge class which knows when to call it properly.
void HighVariable::updateCover(void) const

{
  if (piece == (VariablePiece *)0)
    updateInternalCover();
  else {
    piece->updateIntersections();
    piece->updateCover();
  }
}

/// Only update if flags are marked as \e dirty.
/// Generally if any member Varnode possesses the property, \b this HighVariable should
/// inherit it.  The Varnode::typelock field is not set here, but in updateType().
void HighVariable::updateFlags(void) const

{
  if ((highflags & flagsdirty)==0) return; // flags are up to date

  vector<Varnode *>::const_iterator iter;
  uint4 fl = 0;

  for(iter=inst.begin();iter!=inst.end();++iter)
    fl |= (*iter)->getFlags();

				// Keep these flags
  flags &= (Varnode::mark | Varnode::typelock);
				// Update all but these
  flags |= fl & ~(Varnode::mark | Varnode::directwrite | Varnode::typelock );
  highflags &= ~flagsdirty; // Clear the dirty flag
}

/// Find the member Varnode with the most \e specialized data-type, handling \e bool specially.
/// Boolean data-types are \e specialized in the data-type lattice, but not all byte values are boolean values.
/// Within the Varnode/PcodeOp tree, the \e bool data-type can only propagate to a Varnode if it is verified to
/// only take the boolean values 0 and 1. Since the data-type representative represents the type of all
/// instances, if any instance is not boolean, then the HighVariable cannot be boolean, even though \e bool
/// is more specialized. This method uses Datatype::typeOrderBool() to implement the special handling.
/// \return the representative member
Varnode *HighVariable::getTypeRepresentative(void) const

{
  vector<Varnode *>::const_iterator iter;
  Varnode *vn,*rep;
  
  iter = inst.begin();
  rep = *iter;
  ++iter;
  for(;iter!=inst.end();++iter) {
    vn = *iter;
    if (rep->isTypeLock() != vn->isTypeLock()) {
      if (vn->isTypeLock())
	rep = vn;
    }
    else if (0>vn->getType()->typeOrderBool(*rep->getType()))
      rep = vn;
  }
  return rep;
}

/// Only update if the data-type is marked as \e dirty.
/// Get the most locked, most specific data-type from member Varnode objects.
void HighVariable::updateType(void) const

{
  Varnode *vn;

  if ((highflags&typedirty)==0) return; // Type is up to date
  highflags &= ~typedirty; // Mark type as clean
  if ((highflags & type_finalized)!=0) return;	// Type has been finalized
  vn = getTypeRepresentative();

  type = vn->getType();
  if (type->hasStripped() && type->getMetatype() != TYPE_PARTIALUNION)
    type = type->getStripped();

				// Update lock flags
  flags &= ~Varnode::typelock;
  if (vn->isTypeLock())
    flags |= Varnode::typelock;
}

void HighVariable::updateSymbol(void) const

{
  if ((highflags & symboldirty)==0) return; // flags are up to date
  highflags &= ~((uint4)symboldirty);
  vector<Varnode *>::const_iterator iter;
  symbol = (Symbol *)0;

  for(iter=inst.begin();iter!=inst.end();++iter) {
    Varnode *vn = *iter;
    if (vn->getSymbolEntry() != (SymbolEntry *)0) {
      setSymbol(vn);
      return;
    }
  }
}

/// Compare two Varnode objects based just on their storage address
/// \param a is the first Varnode to compare
/// \param b is the second Varnode
/// \return \b true if the first Varnode should be ordered before the second
bool HighVariable::compareJustLoc(const Varnode *a,const Varnode *b)

{
  return (a->getAddr() < b->getAddr());
}

/// Given two Varnode (members), sort them based on naming properties:
///  - A Varnode with an assigned name is preferred
///  - An \e unaffected Varnode is preferred
///  - A global Varnode is preferred
///  - An \e input Varnode is preferred
///  - An \e address \e tied Varnode is preferred
///  - A non-temporary Varnode is preferred
///  - A written Varnode is preferred
///  - An earlier Varnode is preferred
///
/// \return \b true if the second Varnode's name would override the first's
bool HighVariable::compareName(Varnode *vn1,Varnode *vn2)

{
  if (vn1->isNameLock()) return false; // Check for namelocks
  if (vn2->isNameLock()) return true;

  if (vn1->isUnaffected() != vn2->isUnaffected()) // Prefer unaffected
    return vn2->isUnaffected();
  if (vn1->isPersist() != vn2->isPersist()) // Prefer persistent
    return vn2->isPersist();
  if (vn1->isInput() != vn2->isInput())	// Prefer an input
    return vn2->isInput();
  if (vn1->isAddrTied() != vn2->isAddrTied()) // Prefer address tied
    return vn2->isAddrTied();

  // Prefer NOT internal
  if ((vn1->getSpace()->getType() != IPTR_INTERNAL)&&
      (vn2->getSpace()->getType() == IPTR_INTERNAL))
    return false;
  if ((vn1->getSpace()->getType() == IPTR_INTERNAL)&&
      (vn2->getSpace()->getType() != IPTR_INTERNAL))
    return true;
  if (vn1->isWritten() != vn2->isWritten()) // Prefer written
    return vn2->isWritten();
  if (!vn1->isWritten())
    return false;
  // Prefer earlier
  if (vn1->getDef()->getTime() != vn2->getDef()->getTime())
    return (vn2->getDef()->getTime() < vn1->getDef()->getTime());
  return false;
}

/// Members are scored based the properties that are most dominating in choosing a name.
/// \return the highest scoring Varnode member
Varnode *HighVariable::getNameRepresentative(void) const

{
  if ((highflags & namerepdirty)==0)
    return nameRepresentative;		// Name representative is up to date
  highflags &= ~namerepdirty;

  vector<Varnode *>::const_iterator iter;
  Varnode *vn;

  iter = inst.begin();
  nameRepresentative = *iter;
  ++iter;
  for(;iter!=inst.end();++iter) {
    vn = *iter;
    if (compareName(nameRepresentative,vn))
      nameRepresentative = vn;
  }
  return nameRepresentative;
}

/// Search for the given Varnode and cut it out of the list, marking all properties as \e dirty.
/// \param vn is the given Varnode member to remove
void HighVariable::remove(Varnode *vn)

{
  vector<Varnode *>::iterator iter;

  iter = lower_bound(inst.begin(),inst.end(),vn,compareJustLoc);
  for(;iter!=inst.end();++iter) {
    if (*iter == vn) {
      inst.erase(iter);
      highflags |= (flagsdirty|namerepdirty|coverdirty|typedirty);
      if (vn->getSymbolEntry() != (SymbolEntry *)0)
	highflags |= symboldirty;
      if (piece != (VariablePiece *)0)
	piece->markExtendCoverDirty();
      return;
    }
  }
}

/// Assuming there is a Symbol attached to \b this, run through the Varnode members
/// until we find one with a SymbolEntry corresponding to the Symbol and return it.
/// \return the SymbolEntry that mapped the Symbol to \b this or null if no Symbol is attached
SymbolEntry *HighVariable::getSymbolEntry(void) const

{
  for(int4 i=0;i<inst.size();++i) {
    SymbolEntry *entry = inst[i]->getSymbolEntry();
    if (entry != (SymbolEntry *)0 && entry->getSymbol() == symbol)
      return entry;
  }
  return (SymbolEntry *)0;
}

/// The data-type its dirtying mechanism is disabled.  The data-type will not change, unless
/// this method is called again.
/// \param tp is the data-type to set
void HighVariable::finalizeDatatype(Datatype *tp)

{
  type = tp;
  highflags |= type_finalized;
}

/// If one of the HighVariables is already in a group, the other HighVariable is added to this group.
/// \param off is the relative byte offset of \b this with the other HighVariable
/// \param hi2 is the other HighVariable
void HighVariable::groupWith(int4 off,HighVariable *hi2)

{
  if (piece == (VariablePiece *)0 && hi2->piece == (VariablePiece *)0) {
    hi2->piece = new VariablePiece(hi2,0);
    piece = new VariablePiece(this,off,hi2);
    hi2->piece->markIntersectionDirty();
    return;
  }
  if (piece == (VariablePiece *)0) {
    if ((hi2->highflags & intersectdirty) == 0)
      hi2->piece->markIntersectionDirty();
    highflags |= intersectdirty | extendcoverdirty;
    off += hi2->piece->getOffset();
    piece = new VariablePiece(this,off,hi2);
  }
  else if (hi2->piece == (VariablePiece *)0) {
    int4 hi2Off = piece->getOffset() - off;
    if (hi2Off < 0) {
      piece->adjustOffset(-hi2Off);
      hi2Off = 0;
    }
    if ((highflags & intersectdirty) == 0)
      piece->markIntersectionDirty();
    hi2->highflags |= intersectdirty | extendcoverdirty;
    hi2->piece = new VariablePiece(hi2,hi2Off,this);
  }
  else {
    throw LowlevelError("Cannot group HighVariables that are already grouped");
  }
}

/// The lists of members are merged and the other HighVariable is deleted.
/// \param tv2 is the other HighVariable to merge into \b this
/// \param isspeculative is \b true to keep the new members in separate \e merge classes
void HighVariable::mergeInternal(HighVariable *tv2,bool isspeculative)

{
  int4 i;

  highflags |= (flagsdirty|namerepdirty|typedirty);
  if (tv2->symbol != (Symbol *)0) {		// Check if we inherit a Symbol
    if ((tv2->highflags & symboldirty)==0) {
      symbol = tv2->symbol;			// Overwrite our Symbol (assume it is the same)
      symboloffset = tv2->symboloffset;
      highflags &= ~((uint4)symboldirty);	// Mark that we are not symbol dirty
    }
  }

  if (isspeculative) {
    for(i=0;i<tv2->inst.size();++i) {
      Varnode *vn = tv2->inst[i];
      vn->setHigh(this,vn->getMergeGroup() + numMergeClasses);
    }
    numMergeClasses += tv2->numMergeClasses;
  }
  else {
    if ((numMergeClasses!=1)||(tv2->numMergeClasses!=1))
      throw LowlevelError("Making a non-speculative merge after speculative merges have occurred");
    for(i=0;i<tv2->inst.size();++i) {
      Varnode *vn = tv2->inst[i];
      vn->setHigh(this,vn->getMergeGroup());
    }
  }
  vector<Varnode *> instcopy(inst);
  inst.resize(inst.size()+tv2->inst.size(),(Varnode *)0);
  std::merge(instcopy.begin(),instcopy.end(),tv2->inst.begin(),tv2->inst.end(),inst.begin(),compareJustLoc);
  tv2->inst.clear();

  if (((highflags&coverdirty)==0)&&((tv2->highflags&coverdirty)==0))
    internalCover.merge(tv2->internalCover);
  else
    highflags |= coverdirty;

  delete tv2;
}

/// The HighVariables are merged internally as with mergeInternal.  If \b this is part of a VariableGroup,
/// extended covers of the group may be affected.  If both HighVariables are part of separate groups,
/// the groups are combined into one, which may induce additional HighVariable pairs within the group to be merged.
/// In all cases, the other HighVariable is deleted.
/// \param tv2 is the other HighVariable to merge into \b this
/// \param isspeculative is \b true to keep the new members in separate \e merge classes
void HighVariable::merge(HighVariable *tv2,bool isspeculative)

{
  if (tv2 == this) return;

  if (piece == (VariablePiece *)0 && tv2->piece == (VariablePiece *)0) {
    mergeInternal(tv2,isspeculative);
    return;
  }
  if (tv2->piece == (VariablePiece *)0) {
    // Keep group that this is already in
    piece->markExtendCoverDirty();
    mergeInternal(tv2,isspeculative);
    return;
  }
  if (piece == (VariablePiece *)0) {
    // Move ownership of the VariablePiece object from the HighVariable that will be freed
    transferPiece(tv2);
    piece->markExtendCoverDirty();
    mergeInternal(tv2,isspeculative);
    return;
  }
  // Reaching here both HighVariables are part of a group
  throw LowlevelError("Merging variables in separate groups not supported");
//  vector<HighVariable *> mergePairs;
//  piece->combineOtherGroup(tv2->piece, mergePairs);
//  for(int4 i=0;i<mergePairs.size();i+=2) {
//    HighVariable *high1 = mergePairs[i];
//    HighVariable *high2 = mergePairs[i+1];
// // Need to deal with cached intersect tests herev
//    high1->mergeInternal(high2, isspeculative);
//  }
//  piece->markIntersectionDirty();
}

/// All Varnode objects are assigned a HighVariable, including those that don't get names like
/// indirect variables, constants, and annotations.  Determine if \b this, as inherited from its
/// member Varnodes, can have a name.
/// \return \b true if \b this can have a name
bool HighVariable::hasName(void) const

{
  bool indirectonly = true;
  for(int4 i=0;i<inst.size();++i) {
    Varnode *vn = inst[i];
    if (!vn->hasCover()) {
      if (inst.size() > 1)
	throw LowlevelError("Non-coverable varnode has been merged");
      return false;
    }
    if (vn->isImplied()) {
      if (inst.size() > 1)
	throw LowlevelError("Implied varnode has been merged");
      return false;
    }
    if (!vn->isIndirectOnly())
      indirectonly = false;
  }
  if (isUnaffected()) {
    if (!isInput()) return false;
    if (indirectonly) return false;
    Varnode *vn = getInputVarnode();
    if (!vn->isIllegalInput()) { // A leftover unaff illegal input gets named
      if (vn->isSpacebase())	// A legal input, unaff, gets named
	return false;		// Unless it is the stackpointer
    }
  }
  return true;
}

/// This should only be called if isAddrTied() returns \b true. If there is no address tied
/// member, this will throw an exception.
/// \return the first address tied member
Varnode *HighVariable::getTiedVarnode(void) const

{
  int4 i;

  for(i=0;i<inst.size();++i)
    if (inst[i]->isAddrTied())
      return inst[i];

  throw LowlevelError("Could not find address-tied varnode");
}

/// This should only be called if isInput() returns \b true. If there is no input
/// member, this will throw an exception.
/// \return the input Varnode member
Varnode *HighVariable::getInputVarnode(void) const

{
  for(int4 i=0;i<inst.size();++i)
    if (inst[i]->isInput())
      return inst[i];
  throw LowlevelError("Could not find input varnode");
}

/// This is generally used for debug purposes.
/// \param s is the output stream
void HighVariable::printInfo(ostream &s) const

{
  vector<Varnode *>::const_iterator viter;
  Varnode *vn;

  updateType();
  if (symbol == (Symbol *)0) {
    s << "Variable: UNNAMED" << endl;
  }
  else {
    s << "Variable: " << symbol->getName();
    if (symboloffset!=-1)
      s << "(partial)";
    s << endl;
  }
  s << "Type: ";
  type->printRaw(s);
  s << "\n\n";
				
  for(viter=inst.begin();viter!=inst.end();++viter) {
    vn = *viter;
    s << dec << vn->getMergeGroup() << ": ";
    vn->printInfo(s);
  }
}

/// Find the index, for use with getInstance(), that will retrieve the given Varnode member
/// \param vn is the given Varnode member
/// \return the index of the member or -1 if it is not a member
int4 HighVariable::instanceIndex(const Varnode *vn) const

{
  int4 i;

  for(i=0;i<inst.size();++i)
    if (inst[i] == vn) return i;

  return -1;
}

/// \param encoder is the stream encoder
void HighVariable::encode(Encoder &encoder) const

{
  Varnode *vn = getNameRepresentative(); // Get representative varnode
  encoder.openElement(ELEM_HIGH);
  encoder.writeUnsignedInteger(ATTRIB_REPREF, vn->getCreateIndex());
  if (isSpacebase()||isImplied()) // This is a special variable
    encoder.writeString(ATTRIB_CLASS, "other");
  else if (isPersist()&&isAddrTied()) // Global variable
    encoder.writeString(ATTRIB_CLASS, "global");
  else if (isConstant())
    encoder.writeString(ATTRIB_CLASS, "constant");
  else if (!isPersist() && (symbol != (Symbol *)0)) {
    if (symbol->getCategory() == Symbol::function_parameter)
      encoder.writeString(ATTRIB_CLASS, "param");
    else
      encoder.writeString(ATTRIB_CLASS, "local");
  }
  else {
    encoder.writeString(ATTRIB_CLASS, "other");
  }
  if (isTypeLock())
    encoder.writeBool(ATTRIB_TYPELOCK, true);
  if (symbol != (Symbol *)0) {
    encoder.writeUnsignedInteger(ATTRIB_SYMREF, symbol->getId());
    if (symboloffset >= 0)
      encoder.writeSignedInteger(ATTRIB_OFFSET, symboloffset);
  }
  getType()->encode(encoder);
  for(int4 j=0;j<inst.size();++j) {
    encoder.openElement(ELEM_ADDR);
    encoder.writeUnsignedInteger(ATTRIB_REF, inst[j]->getCreateIndex());
    encoder.closeElement(ELEM_ADDR);
  }
  encoder.closeElement(ELEM_HIGH);
}

/// Given a Varnode at the root of an expression, we collect all the \e explicit HighVariables
/// involved in the expression.  This should only be run after \e explicit and \e implicit
/// properties have been computed on Varnodes.  The expression is traced back from the root
/// until explicit Varnodes are encountered; then their HighVariable is marked and added to the list.
/// The routine returns a value based on PcodeOps encountered in the expression:
///   - 1 for call instructions
///   - 2 for LOAD instructions
///   - 3 for both call and LOAD
///   - 0 for no calls or LOADS
///
/// \param vn is the given root Varnode of the expression
/// \param highList will hold the collected HighVariables
/// \return a value based on call and LOAD instructions in the expression
int4 HighVariable::markExpression(Varnode *vn,vector<HighVariable *> &highList)

{
  HighVariable *high = vn->getHigh();
  high->setMark();
  highList.push_back(high);
  int4 retVal = 0;
  if (!vn->isWritten()) return retVal;

  vector<PcodeOpNode> path;
  PcodeOp *op = vn->getDef();
  if (op->isCall())
    retVal |= 1;
  if (op->code() == CPUI_LOAD)
    retVal |= 2;
  path.push_back(PcodeOpNode(op,0));
  while(!path.empty()) {
    PcodeOpNode &node(path.back());
    if (node.op->numInput() <= node.slot) {
      path.pop_back();
      continue;
    }
    Varnode *curVn = node.op->getIn(node.slot);
    node.slot += 1;
    if (curVn->isAnnotation()) continue;
    if (curVn->isExplicit()) {
      high = curVn->getHigh();
      if (high->isMark()) continue;	// Already in the list
      high->setMark();
      highList.push_back(high);
      continue;				// Truncate at explicit
    }
    if (!curVn->isWritten()) continue;
    op = curVn->getDef();
    if (op->isCall())
      retVal |= 1;
    if (op->code() == CPUI_LOAD)
      retVal |= 2;
    path.push_back(PcodeOpNode(curVn->getDef(),0));
  }
  return retVal;
}

#ifdef MERGEMULTI_DEBUG
/// \brief Check that there are no internal Cover intersections within \b this
///
/// Look for any pair of Varnodes whose covers intersect, but they are not
/// COPY shadows.  Throw an exception in this case.
void HighVariable::verifyCover(void) const

{
  Cover accumCover;

  for(int4 i=0;i<inst.size();++i) {
    Varnode *vn = inst[i];
    if (accumCover.intersect(*vn->getCover()) == 2) {
      for(int4 j=0;j<i;++j) {
	Varnode *otherVn = inst[j];
	if (otherVn->getCover()->intersect(*vn->getCover())==2) {
	  if (!otherVn->copyShadow(vn))
	    throw LowlevelError("HighVariable has internal intersection");
	}
      }
    }
    accumCover.merge(*vn->getCover());
  }
}
#endif
