// Created on: 2013-08-27
// Created by: Denis BOGOLEPOV
// Copyright (c) 2013 OPEN CASCADE SAS
//
// This file is part of Open CASCADE Technology software library.
//
// This library is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License version 2.1 as published
// by the Free Software Foundation, with special exception defined in the file
// OCCT_LGPL_EXCEPTION.txt. Consult the file LICENSE_LGPL_21.txt included in OCCT
// distribution for complete text of the license and disclaimer of any warranty.
//
// Alternatively, this file may be used under the terms of Open CASCADE
// commercial license or contractual agreement.

#include <NCollection_Mat4.hxx>
#include <OpenGl_ArbFBO.hxx>
#include <OpenGl_FrameBuffer.hxx>
#include <OpenGl_Texture.hxx>
#include <OpenGl_VertexBuffer.hxx>
#include <OpenGl_View.hxx>
#include <OpenGl_Workspace.hxx>
#include <OSD_File.hxx>
#include <OSD_Protection.hxx>
#include <Standard_Assert.hxx>

using namespace OpenGl_Raytrace;

//! Use this macro to output ray-tracing debug info
// #define RAY_TRACE_PRINT_INFO

#ifdef RAY_TRACE_PRINT_INFO
  #include <OSD_Timer.hxx>
#endif

// =======================================================================
// function : MatVecMult
// purpose  : Multiples 4x4 matrix by 4D vector
// =======================================================================
template<typename T>
BVH_Vec4f MatVecMult (const T m[16], const BVH_Vec4f& v)
{
  return BVH_Vec4f (
    static_cast<float> (m[ 0] * v.x() + m[ 4] * v.y() +
                        m[ 8] * v.z() + m[12] * v.w()),
    static_cast<float> (m[ 1] * v.x() + m[ 5] * v.y() +
                        m[ 9] * v.z() + m[13] * v.w()),
    static_cast<float> (m[ 2] * v.x() + m[ 6] * v.y() +
                        m[10] * v.z() + m[14] * v.w()),
    static_cast<float> (m[ 3] * v.x() + m[ 7] * v.y() +
                        m[11] * v.z() + m[15] * v.w()));
}

// =======================================================================
// function : UpdateRaytraceGeometry
// purpose  : Updates 3D scene geometry for ray tracing
// =======================================================================
Standard_Boolean OpenGl_Workspace::UpdateRaytraceGeometry (Standard_Boolean theCheck)
{
  if (myView.IsNull())
    return Standard_False;

  // Note: In 'check' mode the scene geometry is analyzed for modifications
  // This is light-weight procedure performed for each frame

  if (!theCheck)
  {
    myRaytraceGeometry.Clear();

    myIsRaytraceDataValid = Standard_False;
  }
  else
  {
    if (myLayersModificationStatus != myView->LayerList().ModificationState())
    {
      return UpdateRaytraceGeometry (Standard_False);
    }
  }

  Standard_ShortReal* aTransform (NULL);

  // The set of processed structures (reflected to ray-tracing)
  // This set is used to remove out-of-date records from the
  // hash map of structures
  std::set<const OpenGl_Structure*> anElements;

  const OpenGl_LayerList& aList = myView->LayerList();

  for (OpenGl_SequenceOfLayers::Iterator anLayerIt (aList.Layers()); anLayerIt.More(); anLayerIt.Next())
  {
    const OpenGl_PriorityList& aPriorityList = anLayerIt.Value();

    if (aPriorityList.NbStructures() == 0)
      continue;

    const OpenGl_ArrayOfStructure& aStructArray = aPriorityList.ArrayOfStructures();

    for (Standard_Integer anIndex = 0; anIndex < aStructArray.Length(); ++anIndex)
    {
      OpenGl_SequenceOfStructure::Iterator aStructIt;

      for (aStructIt.Init (aStructArray (anIndex)); aStructIt.More(); aStructIt.Next())
      {
        const OpenGl_Structure* aStructure = aStructIt.Value();

        if (theCheck)
        {
          if (CheckRaytraceStructure (aStructure))
          {
            return UpdateRaytraceGeometry (Standard_False);
          }
        }
        else
        {
          if (!aStructure->IsRaytracable())
            continue;

          if (aStructure->Transformation()->mat != NULL)
          {
            if (aTransform == NULL)
              aTransform = new Standard_ShortReal[16];

            for (Standard_Integer i = 0; i < 4; ++i)
              for (Standard_Integer j = 0; j < 4; ++j)
              {
                aTransform[j * 4 + i] = aStructure->Transformation()->mat[i][j];
              }
          }

          AddRaytraceStructure (aStructure, aTransform, anElements);
        }
      }
    }
  }

  if (!theCheck)
  {
    // Actualize the hash map of structures -- remove out-of-date records
    std::map<const OpenGl_Structure*, Standard_Size>::iterator anIter = myStructureStates.begin();

    while (anIter != myStructureStates.end())
    {
      if (anElements.find (anIter->first) == anElements.end())
      {
        myStructureStates.erase (anIter++);
      }
      else
      {
        ++anIter;
      }
    }

    // Actualize OpenGL layer list state
    myLayersModificationStatus = myView->LayerList().ModificationState();

    // Rebuild bottom-level and high-level BVHs
    myRaytraceGeometry.ProcessAcceleration();

    const Standard_ShortReal aMinRadius = Max (fabs (myRaytraceGeometry.Box().CornerMin().x()), Max (
      fabs (myRaytraceGeometry.Box().CornerMin().y()), fabs (myRaytraceGeometry.Box().CornerMin().z())));
    const Standard_ShortReal aMaxRadius = Max (fabs (myRaytraceGeometry.Box().CornerMax().x()), Max (
      fabs (myRaytraceGeometry.Box().CornerMax().y()), fabs (myRaytraceGeometry.Box().CornerMax().z())));

    myRaytraceSceneRadius = 2.f /* scale factor */ * Max (aMinRadius, aMaxRadius);

    const BVH_Vec4f aSize = myRaytraceGeometry.Box().Size();

    myRaytraceSceneEpsilon = Max (1e-4f, 1e-4f * sqrtf (
      aSize.x() * aSize.x() + aSize.y() * aSize.y() + aSize.z() * aSize.z()));

    return UploadRaytraceData();
  }

  delete [] aTransform;

  return Standard_True;
}

// =======================================================================
// function : CheckRaytraceStructure
// purpose  :  Checks to see if the structure is modified
// =======================================================================
Standard_Boolean OpenGl_Workspace::CheckRaytraceStructure (const OpenGl_Structure* theStructure)
{
  if (!theStructure->IsRaytracable())
  {
    // Checks to see if all ray-tracable elements were
    // removed from the structure
    if (theStructure->ModificationState() > 0)
    {
      theStructure->ResetModificationState();
      return Standard_True;
    }

    return Standard_False;
  }

  std::map<const OpenGl_Structure*, Standard_Size>::iterator aStructState = myStructureStates.find (theStructure);

  if (aStructState != myStructureStates.end())
    return aStructState->second != theStructure->ModificationState();

  return Standard_True;
}

// =======================================================================
// function : CreateMaterial
// purpose  : Creates ray-tracing material properties
// =======================================================================
void CreateMaterial (const OPENGL_SURF_PROP& theProp, OpenGl_RaytraceMaterial& theMaterial)
{
  const float* aSrcAmb = theProp.isphysic ? theProp.ambcol.rgb : theProp.matcol.rgb;
  theMaterial.Ambient = BVH_Vec4f (aSrcAmb[0] * theProp.amb,
                                   aSrcAmb[1] * theProp.amb,
                                   aSrcAmb[2] * theProp.amb,
                                   1.0f);

  const float* aSrcDif = theProp.isphysic ? theProp.difcol.rgb : theProp.matcol.rgb;
  theMaterial.Diffuse = BVH_Vec4f (aSrcDif[0] * theProp.diff,
                                   aSrcDif[1] * theProp.diff,
                                   aSrcDif[2] * theProp.diff,
                                   1.0f);

  const float aDefSpecCol[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const float* aSrcSpe = theProp.isphysic ? theProp.speccol.rgb : aDefSpecCol;
  theMaterial.Specular = BVH_Vec4f (aSrcSpe[0] * theProp.spec,
                                    aSrcSpe[1] * theProp.spec,
                                    aSrcSpe[2] * theProp.spec,
                                    theProp.shine);

  const float* aSrcEms = theProp.isphysic ? theProp.emscol.rgb : theProp.matcol.rgb;
  theMaterial.Emission = BVH_Vec4f (aSrcEms[0] * theProp.emsv,
                                    aSrcEms[1] * theProp.emsv,
                                    aSrcEms[2] * theProp.emsv,
                                    1.0f);

  // Note: Here we use sub-linear transparency function
  // to produce realistic-looking transparency effect
  theMaterial.Transparency = BVH_Vec4f (powf (theProp.trans, 0.75f),
                                        1.f - theProp.trans,
                                        1.f,
                                        1.f);

  const float aMaxRefl = Max (theMaterial.Diffuse.x() + theMaterial.Specular.x(),
                         Max (theMaterial.Diffuse.y() + theMaterial.Specular.y(),
                              theMaterial.Diffuse.z() + theMaterial.Specular.z()));

  const float aReflectionScale = 0.75f / aMaxRefl;

  theMaterial.Reflection = BVH_Vec4f (theProp.speccol.rgb[0] * theProp.spec * aReflectionScale,
                                      theProp.speccol.rgb[1] * theProp.spec * aReflectionScale,
                                      theProp.speccol.rgb[2] * theProp.spec * aReflectionScale,
                                      0.f);
}

// =======================================================================
// function : AddRaytraceStructure
// purpose  : Adds OpenGL structure to ray-traced scene geometry
// =======================================================================
Standard_Boolean OpenGl_Workspace::AddRaytraceStructure (const OpenGl_Structure* theStructure,
  const Standard_ShortReal* theTransform, std::set<const OpenGl_Structure*>& theElements)
{
  theElements.insert (theStructure);

  if (!theStructure->IsVisible())
  {
    myStructureStates[theStructure] = theStructure->ModificationState();
    return Standard_True;
  }

  // Get structure material
  Standard_Integer aStructMatID = -1;

  if (theStructure->AspectFace() != NULL)
  {
    aStructMatID = static_cast<Standard_Integer> (myRaytraceGeometry.Materials.size());

    OpenGl_RaytraceMaterial aStructMaterial;
    CreateMaterial (theStructure->AspectFace()->IntFront(), aStructMaterial);

    myRaytraceGeometry.Materials.push_back (aStructMaterial);
  }

  for (OpenGl_Structure::GroupIterator aGroupIter (theStructure->DrawGroups()); aGroupIter.More(); aGroupIter.Next())
  {
    // Get group material
    Standard_Integer aGroupMatID = -1;
    if (aGroupIter.Value()->AspectFace() != NULL)
    {
      aGroupMatID = static_cast<Standard_Integer> (myRaytraceGeometry.Materials.size());

      OpenGl_RaytraceMaterial aGroupMaterial;
      CreateMaterial (aGroupIter.Value()->AspectFace()->IntFront(), aGroupMaterial);

      myRaytraceGeometry.Materials.push_back (aGroupMaterial);
    }

    Standard_Integer aMatID = aGroupMatID < 0 ? aStructMatID : aGroupMatID;

    if (aMatID < 0)
    {
      aMatID = static_cast<Standard_Integer> (myRaytraceGeometry.Materials.size());

      myRaytraceGeometry.Materials.push_back (OpenGl_RaytraceMaterial());
    }

    // Add OpenGL elements from group (extract primitives arrays and aspects)
    for (const OpenGl_ElementNode* aNode = aGroupIter.Value()->FirstNode(); aNode != NULL; aNode = aNode->next)
    {
      OpenGl_AspectFace* anAspect = dynamic_cast<OpenGl_AspectFace*> (aNode->elem);
      if (anAspect != NULL)
      {
        aMatID = static_cast<Standard_Integer> (myRaytraceGeometry.Materials.size());

        OpenGl_RaytraceMaterial aMaterial;
        CreateMaterial (anAspect->IntFront(), aMaterial);

        myRaytraceGeometry.Materials.push_back (aMaterial);
      }
      else
      {
        OpenGl_PrimitiveArray* aPrimArray = dynamic_cast<OpenGl_PrimitiveArray*> (aNode->elem);
        if (aPrimArray != NULL)
        {
          NCollection_Handle<BVH_Object<Standard_ShortReal, 4> > aSet =
            AddRaytracePrimitiveArray (aPrimArray->PArray(), aMatID, theTransform);

          if (!aSet.IsNull())
            myRaytraceGeometry.Objects().Append (aSet);
        }
      }
    }
  }

  Standard_ShortReal* aTransform (NULL);

  // Process all connected OpenGL structures
  for (OpenGl_ListOfStructure::Iterator anIts (theStructure->ConnectedStructures()); anIts.More(); anIts.Next())
  {
    if (anIts.Value()->Transformation()->mat != NULL)
    {
      Standard_ShortReal* aTransform = new Standard_ShortReal[16];

      for (Standard_Integer i = 0; i < 4; ++i)
        for (Standard_Integer j = 0; j < 4; ++j)
        {
          aTransform[j * 4 + i] =
            anIts.Value()->Transformation()->mat[i][j];
        }
    }

    if (anIts.Value()->IsRaytracable())
      AddRaytraceStructure (anIts.Value(), aTransform != NULL ? aTransform : theTransform, theElements);
  }

  delete[] aTransform;

  myStructureStates[theStructure] = theStructure->ModificationState();

  return Standard_True;
}

// =======================================================================
// function : AddRaytracePrimitiveArray
// purpose  : Adds OpenGL primitive array to ray-traced scene geometry
// =======================================================================
OpenGl_TriangleSet* OpenGl_Workspace::AddRaytracePrimitiveArray (
  const CALL_DEF_PARRAY* theArray, Standard_Integer theMatID, const Standard_ShortReal* theTransform)
{
  if (theArray->type != TelPolygonsArrayType &&
      theArray->type != TelTrianglesArrayType &&
      theArray->type != TelQuadranglesArrayType &&
      theArray->type != TelTriangleFansArrayType &&
      theArray->type != TelTriangleStripsArrayType &&
      theArray->type != TelQuadrangleStripsArrayType)
  {
    return NULL;
  }

  if (theArray->vertices == NULL)
    return NULL;

#ifdef RAY_TRACE_PRINT_INFO
  switch (theArray->type)
  {
    case TelPolygonsArrayType:
      std::cout << "\tAdding TelPolygonsArrayType" << std::endl; break;
    case TelTrianglesArrayType:
      std::cout << "\tAdding TelTrianglesArrayType" << std::endl; break;
    case TelQuadranglesArrayType:
      std::cout << "\tAdding TelQuadranglesArrayType" << std::endl; break;
    case TelTriangleFansArrayType:
      std::cout << "\tAdding TelTriangleFansArrayType" << std::endl; break;
    case TelTriangleStripsArrayType:
      std::cout << "\tAdding TelTriangleStripsArrayType" << std::endl; break;
    case TelQuadrangleStripsArrayType:
      std::cout << "\tAdding TelQuadrangleStripsArrayType" << std::endl; break;
  }
#endif

  OpenGl_TriangleSet* aSet = new OpenGl_TriangleSet;

  {
    aSet->Vertices.reserve (theArray->num_vertexs);

    for (Standard_Integer aVert = 0; aVert < theArray->num_vertexs; ++aVert)
    {
      BVH_Vec4f aVertex (theArray->vertices[aVert].xyz[0],
                         theArray->vertices[aVert].xyz[1],
                         theArray->vertices[aVert].xyz[2],
                         1.f);
      if (theTransform)
        aVertex = MatVecMult (theTransform, aVertex);

      aSet->Vertices.push_back (aVertex);
    }

    aSet->Normals.reserve (theArray->num_vertexs);

    for (Standard_Integer aNorm = 0; aNorm < theArray->num_vertexs; ++aNorm)
    {
      BVH_Vec4f aNormal;

      // Note: In case of absence of normals, the
      // renderer uses generated geometric normals

      if (theArray->vnormals != NULL)
      {
        aNormal = BVH_Vec4f (theArray->vnormals[aNorm].xyz[0],
                             theArray->vnormals[aNorm].xyz[1],
                             theArray->vnormals[aNorm].xyz[2],
                             0.f);

        if (theTransform)
          aNormal = MatVecMult (theTransform, aNormal);
      }

      aSet->Normals.push_back (aNormal);
    }

    if (theArray->num_bounds > 0)
    {
  #ifdef RAY_TRACE_PRINT_INFO
      std::cout << "\tNumber of bounds = " << theArray->num_bounds << std::endl;
  #endif

      Standard_Integer aBoundStart = 0;

      for (Standard_Integer aBound = 0; aBound < theArray->num_bounds; ++aBound)
      {
        const Standard_Integer aVertNum = theArray->bounds[aBound];

  #ifdef RAY_TRACE_PRINT_INFO
        std::cout << "\tAdding indices from bound " << aBound << ": " <<
                                      aBoundStart << " .. " << aVertNum << std::endl;
  #endif

        if (!AddRaytraceVertexIndices (aSet, theArray, aBoundStart, aVertNum, theMatID))
        {
          delete aSet;
          return NULL;
        }

        aBoundStart += aVertNum;
      }
    }
    else
    {
      const Standard_Integer aVertNum = theArray->num_edges > 0 ? theArray->num_edges : theArray->num_vertexs;

  #ifdef RAY_TRACE_PRINT_INFO
        std::cout << "\tAdding indices from array: " << aVertNum << std::endl;
  #endif

      if (!AddRaytraceVertexIndices (aSet, theArray, 0, aVertNum, theMatID))
      {
        delete aSet;
        return NULL;
      }
    }
  }

  if (aSet->Size() != 0)
    aSet->MarkDirty();

  return aSet;
}

// =======================================================================
// function : AddRaytraceVertexIndices
// purpose  : Adds vertex indices to ray-traced scene geometry
// =======================================================================
Standard_Boolean OpenGl_Workspace::AddRaytraceVertexIndices (OpenGl_TriangleSet* theSet,
  const CALL_DEF_PARRAY* theArray, Standard_Integer theOffset, Standard_Integer theCount, Standard_Integer theMatID)
{
  switch (theArray->type)
  {
    case TelTrianglesArrayType:
      return AddRaytraceTriangleArray (theSet, theArray, theOffset, theCount, theMatID);

    case TelQuadranglesArrayType:
      return AddRaytraceQuadrangleArray (theSet, theArray, theOffset, theCount, theMatID);

    case TelTriangleFansArrayType:
      return AddRaytraceTriangleFanArray (theSet, theArray, theOffset, theCount, theMatID);

    case TelTriangleStripsArrayType:
      return AddRaytraceTriangleStripArray (theSet, theArray, theOffset, theCount, theMatID);

    case TelQuadrangleStripsArrayType:
      return AddRaytraceQuadrangleStripArray (theSet, theArray, theOffset, theCount, theMatID);

    default:
      return AddRaytracePolygonArray (theSet, theArray, theOffset, theCount, theMatID);
  }
}

// =======================================================================
// function : AddRaytraceTriangleArray
// purpose  : Adds OpenGL triangle array to ray-traced scene geometry
// =======================================================================
Standard_Boolean OpenGl_Workspace::AddRaytraceTriangleArray (OpenGl_TriangleSet* theSet,
  const CALL_DEF_PARRAY* theArray, Standard_Integer theOffset, Standard_Integer theCount, Standard_Integer theMatID)
{
  if (theCount < 3)
    return Standard_True;

  theSet->Elements.reserve (theSet->Elements.size() + theCount / 3);

  if (theArray->num_edges > 0)
  {
    for (Standard_Integer aVert = theOffset; aVert < theOffset + theCount - 2; aVert += 3)
    {
      theSet->Elements.push_back (BVH_Vec4i (theArray->edges[aVert + 0],
                                             theArray->edges[aVert + 1],
                                             theArray->edges[aVert + 2],
                                             theMatID));
    }
  }
  else
  {
    for (Standard_Integer aVert = theOffset; aVert < theOffset + theCount - 2; aVert += 3)
    {
      theSet->Elements.push_back (BVH_Vec4i (aVert + 0,
                                             aVert + 1,
                                             aVert + 2,
                                             theMatID));
    }
  }

  return Standard_True;
}

// =======================================================================
// function : AddRaytraceTriangleFanArray
// purpose  : Adds OpenGL triangle fan array to ray-traced scene geometry
// =======================================================================
Standard_Boolean OpenGl_Workspace::AddRaytraceTriangleFanArray (OpenGl_TriangleSet* theSet,
  const CALL_DEF_PARRAY* theArray, Standard_Integer theOffset, Standard_Integer theCount, Standard_Integer theMatID)
{
  if (theCount < 3)
    return Standard_True;

  theSet->Elements.reserve (theSet->Elements.size() + theCount - 2);

  if (theArray->num_edges > 0)
  {
    for (Standard_Integer aVert = theOffset; aVert < theOffset + theCount - 2; ++aVert)
    {
      theSet->Elements.push_back (BVH_Vec4i (theArray->edges[theOffset],
                                             theArray->edges[aVert + 1],
                                             theArray->edges[aVert + 2],
                                             theMatID));
    }
  }
  else
  {
    for (Standard_Integer aVert = theOffset; aVert < theOffset + theCount - 2; ++aVert)
    {
      theSet->Elements.push_back (BVH_Vec4i (theOffset,
                                             aVert + 1,
                                             aVert + 2,
                                             theMatID));
    }
  }

  return Standard_True;
}

// =======================================================================
// function : AddRaytraceTriangleStripArray
// purpose  : Adds OpenGL triangle strip array to ray-traced scene geometry
// =======================================================================
Standard_Boolean OpenGl_Workspace::AddRaytraceTriangleStripArray (OpenGl_TriangleSet* theSet,
  const CALL_DEF_PARRAY* theArray, Standard_Integer theOffset, Standard_Integer theCount, Standard_Integer theMatID)
{
  if (theCount < 3)
    return Standard_True;

  theSet->Elements.reserve (theSet->Elements.size() + theCount - 2);

  if (theArray->num_edges > 0)
  {
    for (Standard_Integer aVert = theOffset, aCW = 0; aVert < theOffset + theCount - 2; ++aVert, aCW = (aCW + 1) % 2)
    {
      theSet->Elements.push_back (BVH_Vec4i (theArray->edges[aVert + aCW ? 1 : 0],
                                             theArray->edges[aVert + aCW ? 0 : 1],
                                             theArray->edges[aVert + 2],
                                             theMatID));
    }
  }
  else
  {
    for (Standard_Integer aVert = theOffset, aCW = 0; aVert < theOffset + theCount - 2; ++aVert, aCW = (aCW + 1) % 2)
    {
      theSet->Elements.push_back (BVH_Vec4i (aVert + aCW ? 1 : 0,
                                             aVert + aCW ? 0 : 1,
                                             aVert + 2,
                                             theMatID));
    }
  }

  return Standard_True;
}

// =======================================================================
// function : AddRaytraceQuadrangleArray
// purpose  : Adds OpenGL quad array to ray-traced scene geometry
// =======================================================================
Standard_Boolean OpenGl_Workspace::AddRaytraceQuadrangleArray (OpenGl_TriangleSet* theSet,
  const CALL_DEF_PARRAY* theArray, Standard_Integer theOffset, Standard_Integer theCount, Standard_Integer theMatID)
{
  if (theCount < 4)
    return Standard_True;

  theSet->Elements.reserve (theSet->Elements.size() + theCount / 2);

  if (theArray->num_edges > 0)
  {
    for (Standard_Integer aVert = theOffset; aVert < theOffset + theCount - 3; aVert += 4)
    {
      theSet->Elements.push_back (BVH_Vec4i (theArray->edges[aVert + 0],
                                             theArray->edges[aVert + 1],
                                             theArray->edges[aVert + 2],
                                             theMatID));

      theSet->Elements.push_back (BVH_Vec4i (theArray->edges[aVert + 0],
                                             theArray->edges[aVert + 2],
                                             theArray->edges[aVert + 3],
                                             theMatID));
    }
  }
  else
  {
    for (Standard_Integer aVert = theOffset; aVert < theOffset + theCount - 3; aVert += 4)
    {
      theSet->Elements.push_back (BVH_Vec4i (aVert + 0,
                                             aVert + 1,
                                             aVert + 2,
                                             theMatID));

      theSet->Elements.push_back (BVH_Vec4i (aVert + 0,
                                             aVert + 2,
                                             aVert + 3,
                                             theMatID));
    }
  }

  return Standard_True;
}

// =======================================================================
// function : AddRaytraceQuadrangleStripArray
// purpose  : Adds OpenGL quad strip array to ray-traced scene geometry
// =======================================================================
Standard_Boolean OpenGl_Workspace::AddRaytraceQuadrangleStripArray (OpenGl_TriangleSet* theSet,
  const CALL_DEF_PARRAY* theArray, Standard_Integer theOffset, Standard_Integer theCount, Standard_Integer theMatID)
{
  if (theCount < 4)
    return Standard_True;

  theSet->Elements.reserve (theSet->Elements.size() + 2 * theCount - 6);

  if (theArray->num_edges > 0)
  {
    for (Standard_Integer aVert = theOffset; aVert < theOffset + theCount - 3; aVert += 2)
    {
      theSet->Elements.push_back (BVH_Vec4i (theArray->edges[aVert + 0],
                                             theArray->edges[aVert + 1],
                                             theArray->edges[aVert + 2],
                                             theMatID));

      theSet->Elements.push_back (BVH_Vec4i (theArray->edges[aVert + 1],
                                             theArray->edges[aVert + 3],
                                             theArray->edges[aVert + 2],
                                             theMatID));
    }
  }
  else
  {
    for (Standard_Integer aVert = theOffset; aVert < theOffset + theCount - 3; aVert += 2)
    {
      theSet->Elements.push_back (BVH_Vec4i (aVert + 0,
                                             aVert + 1,
                                             aVert + 2,
                                             theMatID));

      theSet->Elements.push_back (BVH_Vec4i (aVert + 1,
                                             aVert + 3,
                                             aVert + 2,
                                             theMatID));
    }
  }

  return Standard_True;
}

// =======================================================================
// function : AddRaytracePolygonArray
// purpose  : Adds OpenGL polygon array to ray-traced scene geometry
// =======================================================================
Standard_Boolean OpenGl_Workspace::AddRaytracePolygonArray (OpenGl_TriangleSet* theSet,
  const CALL_DEF_PARRAY* theArray, Standard_Integer theOffset, Standard_Integer theCount, Standard_Integer theMatID)
{
  if (theCount < 3)
    return Standard_True;

  theSet->Elements.reserve (theSet->Elements.size() + theCount - 2);

  if (theArray->num_edges > 0)
  {
    for (Standard_Integer aVert = theOffset; aVert < theOffset + theCount - 2; ++aVert)
    {
      theSet->Elements.push_back (BVH_Vec4i (theArray->edges[theOffset],
                                             theArray->edges[aVert + 1],
                                             theArray->edges[aVert + 2],
                                             theMatID));
    }
  }
  else
  {
    for (Standard_Integer aVert = theOffset; aVert < theOffset + theCount - 2; ++aVert)
    {
      theSet->Elements.push_back (BVH_Vec4i (theOffset,
                                             aVert + 1,
                                             aVert + 2,
                                             theMatID));
    }
  }

  return Standard_True;
}

// =======================================================================
// function : UpdateRaytraceLightSources
// purpose  : Updates 3D scene light sources for ray-tracing
// =======================================================================
Standard_Boolean OpenGl_Workspace::UpdateRaytraceLightSources (const GLdouble theInvModelView[16])
{
  myRaytraceGeometry.Sources.clear();

  myRaytraceGeometry.Ambient = BVH_Vec4f (0.0f, 0.0f, 0.0f, 0.0f);

  for (OpenGl_ListOfLight::Iterator anItl (myView->LightList()); anItl.More(); anItl.Next())
  {
    const OpenGl_Light& aLight = anItl.Value();

    if (aLight.Type == Visual3d_TOLS_AMBIENT)
    {
      myRaytraceGeometry.Ambient += BVH_Vec4f (aLight.Color.r(),
                                               aLight.Color.g(),
                                               aLight.Color.b(),
                                               0.0f);
      continue;
    }

    BVH_Vec4f aDiffuse  (aLight.Color.r(),
                         aLight.Color.g(),
                         aLight.Color.b(),
                         1.0f);

    BVH_Vec4f aPosition (-aLight.Direction.x(),
                         -aLight.Direction.y(),
                         -aLight.Direction.z(),
                         0.0f);

    if (aLight.Type != Visual3d_TOLS_DIRECTIONAL)
    {
      aPosition = BVH_Vec4f (aLight.Position.x(),
                             aLight.Position.y(),
                             aLight.Position.z(),
                             1.0f);
    }

    if (aLight.IsHeadlight)
      aPosition = MatVecMult (theInvModelView, aPosition);

    
    myRaytraceGeometry.Sources.push_back (OpenGl_RaytraceLight (aDiffuse, aPosition));
  }

  if (myRaytraceLightSrcTexture.IsNull())  // create light source buffer
  {
    myRaytraceLightSrcTexture = new OpenGl_TextureBufferArb;

    if (!myRaytraceLightSrcTexture->Create (myGlContext))
    {
#ifdef RAY_TRACE_PRINT_INFO
      std::cout << "Error: Failed to create light source buffer" << std::endl;
#endif
      return Standard_False;
    }
  }
  
  if (myRaytraceGeometry.Sources.size() != 0)
  {
    const GLfloat* aDataPtr = myRaytraceGeometry.Sources.front().Packed();
    if (!myRaytraceLightSrcTexture->Init (myGlContext, 4, GLsizei (myRaytraceGeometry.Sources.size() * 2), aDataPtr))
    {
#ifdef RAY_TRACE_PRINT_INFO
      std::cout << "Error: Failed to upload light source buffer" << std::endl;
#endif
      return Standard_False;
    }
  }

  return Standard_True;
}

// =======================================================================
// function : UpdateRaytraceEnvironmentMap
// purpose  : Updates environment map for ray-tracing
// =======================================================================
Standard_Boolean OpenGl_Workspace::UpdateRaytraceEnvironmentMap()
{
  if (myView.IsNull())
    return Standard_False;

  if (myViewModificationStatus == myView->ModificationState())
    return Standard_True;

  for (Standard_Integer anIdx = 0; anIdx < 2; ++anIdx)
  {
    const Handle(OpenGl_ShaderProgram)& aProgram =
      anIdx == 0 ? myRaytraceProgram : myPostFSAAProgram;

    if (!aProgram.IsNull())
    {
      aProgram->Bind (myGlContext);

      if (!myView->TextureEnv().IsNull() && myView->SurfaceDetail() != Visual3d_TOD_NONE)
      {
        myView->TextureEnv()->Bind (
          myGlContext, GL_TEXTURE0 + OpenGl_RT_EnvironmentMapTexture);

        aProgram->SetUniform (myGlContext, "uEnvironmentEnable", 1);
      }
      else
      {
        aProgram->SetUniform (myGlContext, "uEnvironmentEnable", 0);
      }

      aProgram->SetSampler (myGlContext,
        "uEnvironmentMapTexture", OpenGl_RT_EnvironmentMapTexture);
    }
  }

  OpenGl_ShaderProgram::Unbind (myGlContext);

  myViewModificationStatus = myView->ModificationState();

  return Standard_True;
}

// =======================================================================
// function : Source
// purpose  : Returns shader source combined with prefix
// =======================================================================
TCollection_AsciiString OpenGl_Workspace::ShaderSource::Source() const
{
  static const TCollection_AsciiString aVersion = "#version 140";

  if (myPrefix.IsEmpty())
  {
    return aVersion + "\n" + mySource;
  }

  return aVersion + "\n" + myPrefix + "\n" + mySource;
}

// =======================================================================
// function : Load
// purpose  : Loads shader source from specified files
// =======================================================================
void OpenGl_Workspace::ShaderSource::Load (
  const TCollection_AsciiString* theFileNames, const Standard_Integer theCount)
{
  mySource.Clear();

  for (Standard_Integer anIndex = 0; anIndex < theCount; ++anIndex)
  {
    OSD_File aFile (theFileNames[anIndex]);

    Standard_ASSERT_RETURN (aFile.Exists(),
      "Error: Failed to find shader source file", /* none */);

    aFile.Open (OSD_ReadOnly, OSD_Protection());

    TCollection_AsciiString aSource;

    Standard_ASSERT_RETURN (aFile.IsOpen(),
      "Error: Failed to open shader source file", /* none */);

    aFile.Read (aSource, (Standard_Integer) aFile.Size());

    if (!aSource.IsEmpty())
    {
      mySource += TCollection_AsciiString ("\n") + aSource;
    }

    aFile.Close();
  }
}

// =======================================================================
// function : LoadShader
// purpose  : Creates new shader object with specified source
// =======================================================================
Handle(OpenGl_ShaderObject) OpenGl_Workspace::LoadShader (const ShaderSource& theSource, GLenum theType)
{
  Handle(OpenGl_ShaderObject) aShader = new OpenGl_ShaderObject (theType);

  if (!aShader->Create (myGlContext))
  {
    const TCollection_ExtendedString aMessage = "Error: Failed to create shader object";
      
    myGlContext->PushMessage (GL_DEBUG_SOURCE_APPLICATION_ARB,
      GL_DEBUG_TYPE_ERROR_ARB, 0, GL_DEBUG_SEVERITY_HIGH_ARB, aMessage);

    aShader->Release (myGlContext.operator->());

    return Handle(OpenGl_ShaderObject)();
  }

  if (!aShader->LoadSource (myGlContext, theSource.Source()))
  {
    const TCollection_ExtendedString aMessage = "Error: Failed to set shader source";
      
    myGlContext->PushMessage (GL_DEBUG_SOURCE_APPLICATION_ARB,
      GL_DEBUG_TYPE_ERROR_ARB, 0, GL_DEBUG_SEVERITY_HIGH_ARB, aMessage);

    aShader->Release (myGlContext.operator->());

    return Handle(OpenGl_ShaderObject)();
  }

  TCollection_AsciiString aBuildLog;

  if (!aShader->Compile (myGlContext))
  {
    if (aShader->FetchInfoLog (myGlContext, aBuildLog))
    {
      const TCollection_ExtendedString aMessage =
        TCollection_ExtendedString ("Error: Failed to compile shader object:\n") + aBuildLog;

#ifdef RAY_TRACE_PRINT_INFO
      std::cout << aBuildLog << std::endl;
#endif

      myGlContext->PushMessage (GL_DEBUG_SOURCE_APPLICATION_ARB,
        GL_DEBUG_TYPE_ERROR_ARB, 0, GL_DEBUG_SEVERITY_HIGH_ARB, aMessage);
    }
    
    aShader->Release (myGlContext.operator->());

    return Handle(OpenGl_ShaderObject)();
  }

#ifdef RAY_TRACE_PRINT_INFO
  if (aShader->FetchInfoLog (myGlContext, aBuildLog))
  {
    if (!aBuildLog.IsEmpty())
    {
      std::cout << aBuildLog << std::endl;
    }
    else
    {
      std::cout << "Info: shader build log is empty" << std::endl;
    }
  }  
#endif

  return aShader;
}

// =======================================================================
// function : SafeFailBack
// purpose  : Performs safe exit when shaders initialization fails
// =======================================================================
Standard_Boolean OpenGl_Workspace::SafeFailBack (const TCollection_ExtendedString& theMessage)
{
  myGlContext->PushMessage (GL_DEBUG_SOURCE_APPLICATION_ARB,
    GL_DEBUG_TYPE_ERROR_ARB, 0, GL_DEBUG_SEVERITY_HIGH_ARB, theMessage);

  myComputeInitStatus = OpenGl_RT_FAIL;

  ReleaseRaytraceResources();
  
  return Standard_False;
}

// =======================================================================
// function : InitRaytraceResources
// purpose  : Initializes OpenGL/GLSL shader programs
// =======================================================================
Standard_Boolean OpenGl_Workspace::InitRaytraceResources()
{
  Standard_Boolean aToRebuildShaders = Standard_False;

  if (myComputeInitStatus == OpenGl_RT_INIT)
  {
    if (!myIsRaytraceDataValid)
      return Standard_True;

    const Standard_Integer aRequiredStackSize =
      myRaytraceGeometry.HighLevelTreeDepth() + myRaytraceGeometry.BottomLevelTreeDepth();

    if (myTraversalStackSize < aRequiredStackSize)
    {
      myTraversalStackSize = Max (aRequiredStackSize, THE_DEFAULT_STACK_SIZE);

      aToRebuildShaders = Standard_True;
    }
    else
    {
      if (aRequiredStackSize < myTraversalStackSize)
      {
        if (myTraversalStackSize > THE_DEFAULT_STACK_SIZE)
        {
          myTraversalStackSize = Max (aRequiredStackSize, THE_DEFAULT_STACK_SIZE);

          aToRebuildShaders = Standard_True;
        }
      }
    }

    if (aToRebuildShaders)
    {
#ifdef RAY_TRACE_PRINT_INFO
      std::cout << "Info: Rebuild shaders with stack size: " << myTraversalStackSize << std::endl;
#endif

      TCollection_AsciiString aStackSizeStr =
        TCollection_AsciiString ("#define STACK_SIZE ") + TCollection_AsciiString (myTraversalStackSize);

      myRaytraceShaderSource.SetPrefix (aStackSizeStr);
      myPostFSAAShaderSource.SetPrefix (aStackSizeStr);

      if (!myRaytraceShader->LoadSource (myGlContext, myRaytraceShaderSource.Source())
       || !myPostFSAAShader->LoadSource (myGlContext, myPostFSAAShaderSource.Source()))
      {
        return Standard_False;
      }

      if (!myRaytraceShader->Compile (myGlContext)
       || !myPostFSAAShader->Compile (myGlContext))
      {
        return Standard_False;
      }

      if (!myRaytraceProgram->Link (myGlContext)
       || !myPostFSAAProgram->Link (myGlContext))
      {
        return Standard_False;
      }
    }
  }

  if (myComputeInitStatus == OpenGl_RT_NONE)
  {
    if (!myGlContext->IsGlGreaterEqual (3, 1))
    {
      const TCollection_ExtendedString aMessage = "Ray-tracing requires OpenGL 3.1 and higher";

      myGlContext->PushMessage (GL_DEBUG_SOURCE_APPLICATION_ARB,
        GL_DEBUG_TYPE_ERROR_ARB, 0, GL_DEBUG_SEVERITY_HIGH_ARB, aMessage);

      return Standard_False;
    }

    TCollection_AsciiString aFolder = Graphic3d_ShaderProgram::ShadersFolder();

    if (aFolder.IsEmpty())
    {
      const TCollection_ExtendedString aMessage = "Failed to locate shaders directory";
      
      myGlContext->PushMessage (GL_DEBUG_SOURCE_APPLICATION_ARB,
        GL_DEBUG_TYPE_ERROR_ARB, 0, GL_DEBUG_SEVERITY_HIGH_ARB, aMessage);
      
      return Standard_False;
    }

    if (myIsRaytraceDataValid)
    {
      myTraversalStackSize = Max (THE_DEFAULT_STACK_SIZE,
        myRaytraceGeometry.HighLevelTreeDepth() + myRaytraceGeometry.BottomLevelTreeDepth());
    }

    {
      Handle(OpenGl_ShaderObject) aBasicVertShader = LoadShader (
        ShaderSource (aFolder + "/RaytraceBase.vs"), GL_VERTEX_SHADER);

      if (aBasicVertShader.IsNull())
      {
        return SafeFailBack ("Failed to set vertex shader source");
      }

      TCollection_AsciiString aFiles[] = { aFolder + "/RaytraceBase.fs", aFolder + "/RaytraceRender.fs" };

      myRaytraceShaderSource.Load (aFiles, 2);

      TCollection_AsciiString aStackSizeStr =
        TCollection_AsciiString ("#define STACK_SIZE ") + TCollection_AsciiString (myTraversalStackSize);

      myRaytraceShaderSource.SetPrefix (aStackSizeStr);

      myRaytraceShader = LoadShader (myRaytraceShaderSource, GL_FRAGMENT_SHADER);

      if (myRaytraceShader.IsNull())
      {
        aBasicVertShader->Release (myGlContext.operator->());

        return SafeFailBack ("Failed to set ray-trace fragment shader source");
      }

      myRaytraceProgram = new OpenGl_ShaderProgram;

      if (!myRaytraceProgram->Create (myGlContext))
      {
        aBasicVertShader->Release (myGlContext.operator->());

        return SafeFailBack ("Failed to create ray-trace shader program");
      }

      if (!myRaytraceProgram->AttachShader (myGlContext, aBasicVertShader)
       || !myRaytraceProgram->AttachShader (myGlContext, myRaytraceShader))
      {
        aBasicVertShader->Release (myGlContext.operator->());

        return SafeFailBack ("Failed to attach ray-trace shader objects");
      }

      if (!myRaytraceProgram->Link (myGlContext))
      {
        TCollection_AsciiString aLinkLog;

        if (myRaytraceProgram->FetchInfoLog (myGlContext, aLinkLog))
        {
  #ifdef RAY_TRACE_PRINT_INFO
          std::cout << aLinkLog << std::endl;
  #endif
        }

        return SafeFailBack ("Failed to link ray-trace shader program");
      }
    }

    {
      Handle(OpenGl_ShaderObject) aBasicVertShader = LoadShader (
        ShaderSource (aFolder + "/RaytraceBase.vs"), GL_VERTEX_SHADER);

      if (aBasicVertShader.IsNull())
      {
        return SafeFailBack ("Failed to set vertex shader source");
      }

      TCollection_AsciiString aFiles[] = { aFolder + "/RaytraceBase.fs", aFolder + "/RaytraceSmooth.fs" };

      myPostFSAAShaderSource.Load (aFiles, 2);

      TCollection_AsciiString aStackSizeStr =
        TCollection_AsciiString ("#define STACK_SIZE ") + TCollection_AsciiString (myTraversalStackSize);

      myPostFSAAShaderSource.SetPrefix (aStackSizeStr);
    
      myPostFSAAShader = LoadShader (myPostFSAAShaderSource, GL_FRAGMENT_SHADER);

      if (myPostFSAAShader.IsNull())
      {
        aBasicVertShader->Release (myGlContext.operator->());

        return SafeFailBack ("Failed to set FSAA fragment shader source");
      }

      myPostFSAAProgram = new OpenGl_ShaderProgram;

      if (!myPostFSAAProgram->Create (myGlContext))
      {
        aBasicVertShader->Release (myGlContext.operator->());

        return SafeFailBack ("Failed to create FSAA shader program");
      }

      if (!myPostFSAAProgram->AttachShader (myGlContext, aBasicVertShader)
       || !myPostFSAAProgram->AttachShader (myGlContext, myPostFSAAShader))
      {
        aBasicVertShader->Release (myGlContext.operator->());

        return SafeFailBack ("Failed to attach FSAA shader objects");
      }

      if (!myPostFSAAProgram->Link (myGlContext))
      {
        TCollection_AsciiString aLinkLog;

        if (myPostFSAAProgram->FetchInfoLog (myGlContext, aLinkLog))
        {
  #ifdef RAY_TRACE_PRINT_INFO
          std::cout << aLinkLog << std::endl;
  #endif
        }
      
        return SafeFailBack ("Failed to link FSAA shader program");
      }
    }
  }

  if (myComputeInitStatus == OpenGl_RT_NONE || aToRebuildShaders)
  {
    for (Standard_Integer anIndex = 0; anIndex < 2; ++anIndex)
    {
      Handle(OpenGl_ShaderProgram)& aShaderProgram =
        (anIndex == 0) ? myRaytraceProgram : myPostFSAAProgram;

      aShaderProgram->Bind (myGlContext);

      aShaderProgram->SetSampler (myGlContext,
        "uSceneMinPointTexture", OpenGl_RT_SceneMinPointTexture);
      aShaderProgram->SetSampler (myGlContext,
        "uSceneMaxPointTexture", OpenGl_RT_SceneMaxPointTexture);
      aShaderProgram->SetSampler (myGlContext,
        "uSceneNodeInfoTexture", OpenGl_RT_SceneNodeInfoTexture);
      aShaderProgram->SetSampler (myGlContext,
        "uObjectMinPointTexture", OpenGl_RT_ObjectMinPointTexture);
      aShaderProgram->SetSampler (myGlContext,
        "uObjectMaxPointTexture", OpenGl_RT_ObjectMaxPointTexture);
      aShaderProgram->SetSampler (myGlContext,
        "uObjectNodeInfoTexture", OpenGl_RT_ObjectNodeInfoTexture);
      aShaderProgram->SetSampler (myGlContext,
        "uGeometryVertexTexture", OpenGl_RT_GeometryVertexTexture);
      aShaderProgram->SetSampler (myGlContext,
        "uGeometryNormalTexture", OpenGl_RT_GeometryNormalTexture);
      aShaderProgram->SetSampler (myGlContext,
        "uGeometryTriangTexture", OpenGl_RT_GeometryTriangTexture);
      aShaderProgram->SetSampler (myGlContext,
        "uRaytraceMaterialTexture", OpenGl_RT_RaytraceMaterialTexture);
      aShaderProgram->SetSampler (myGlContext,
        "uRaytraceLightSrcTexture", OpenGl_RT_RaytraceLightSrcTexture);

      if (anIndex == 1)
      {
        aShaderProgram->SetSampler (myGlContext,
          "uFSAAInputTexture", OpenGl_RT_FSAAInputTexture);
      }

      myUniformLocations[anIndex][OpenGl_RT_aPosition] =
        aShaderProgram->GetAttributeLocation (myGlContext, "aPosition");

      myUniformLocations[anIndex][OpenGl_RT_uOriginLB] =
        aShaderProgram->GetUniformLocation (myGlContext, "uOriginLB");
      myUniformLocations[anIndex][OpenGl_RT_uOriginRB] =
        aShaderProgram->GetUniformLocation (myGlContext, "uOriginRB");
      myUniformLocations[anIndex][OpenGl_RT_uOriginLT] =
        aShaderProgram->GetUniformLocation (myGlContext, "uOriginLT");
      myUniformLocations[anIndex][OpenGl_RT_uOriginRT] =
        aShaderProgram->GetUniformLocation (myGlContext, "uOriginRT");
      myUniformLocations[anIndex][OpenGl_RT_uDirectLB] =
        aShaderProgram->GetUniformLocation (myGlContext, "uDirectLB");
      myUniformLocations[anIndex][OpenGl_RT_uDirectRB] =
        aShaderProgram->GetUniformLocation (myGlContext, "uDirectRB");
      myUniformLocations[anIndex][OpenGl_RT_uDirectLT] =
        aShaderProgram->GetUniformLocation (myGlContext, "uDirectLT");
      myUniformLocations[anIndex][OpenGl_RT_uDirectRT] =
        aShaderProgram->GetUniformLocation (myGlContext, "uDirectRT");

      myUniformLocations[anIndex][OpenGl_RT_uLightCount] =
        aShaderProgram->GetUniformLocation (myGlContext, "uLightCount");
      myUniformLocations[anIndex][OpenGl_RT_uLightAmbnt] =
        aShaderProgram->GetUniformLocation (myGlContext, "uGlobalAmbient");

      myUniformLocations[anIndex][OpenGl_RT_uSceneRad] =
        aShaderProgram->GetUniformLocation (myGlContext, "uSceneRadius");
      myUniformLocations[anIndex][OpenGl_RT_uSceneEps] =
        aShaderProgram->GetUniformLocation (myGlContext, "uSceneEpsilon");

      myUniformLocations[anIndex][OpenGl_RT_uShadEnabled] =
        aShaderProgram->GetUniformLocation (myGlContext, "uShadowsEnable");
      myUniformLocations[anIndex][OpenGl_RT_uReflEnabled] =
        aShaderProgram->GetUniformLocation (myGlContext, "uReflectionsEnable");

      myUniformLocations[anIndex][OpenGl_RT_uOffsetX] =
        aShaderProgram->GetUniformLocation (myGlContext, "uOffsetX");
      myUniformLocations[anIndex][OpenGl_RT_uOffsetY] =
        aShaderProgram->GetUniformLocation (myGlContext, "uOffsetY");
      myUniformLocations[anIndex][OpenGl_RT_uSamples] =
        aShaderProgram->GetUniformLocation (myGlContext, "uSamples");
    }

    OpenGl_ShaderProgram::Unbind (myGlContext);
  }

  if (myComputeInitStatus != OpenGl_RT_NONE)
  {
    return myComputeInitStatus == OpenGl_RT_INIT;
  }

  if (myRaytraceFBO1.IsNull())
  {
    myRaytraceFBO1 = new OpenGl_FrameBuffer;
  }

  if (myRaytraceFBO2.IsNull())
  {
    myRaytraceFBO2 = new OpenGl_FrameBuffer;
  }

  const GLfloat aVertices[] = { -1.f, -1.f,  0.f,
                                -1.f,  1.f,  0.f,
                                 1.f,  1.f,  0.f,
                                 1.f,  1.f,  0.f,
                                 1.f, -1.f,  0.f,
                                -1.f, -1.f,  0.f };

  myRaytraceScreenQuad.Init (myGlContext, 3, 6, aVertices);

  myComputeInitStatus = OpenGl_RT_INIT; // initialized in normal way
  
  return Standard_True;
}

// =======================================================================
// function : NullifyResource
// purpose  :
// =======================================================================
inline void NullifyResource (const Handle(OpenGl_Context)& theContext,
                             Handle(OpenGl_Resource)&      theResource)
{
  if (!theResource.IsNull())
  {
    theResource->Release (theContext.operator->());
    theResource.Nullify();
  }
}

// =======================================================================
// function : ReleaseRaytraceResources
// purpose  : Releases OpenGL/GLSL shader programs
// =======================================================================
void OpenGl_Workspace::ReleaseRaytraceResources()
{
  NullifyResource (myGlContext, myRaytraceFBO1);
  NullifyResource (myGlContext, myRaytraceFBO2);

  NullifyResource (myGlContext, myRaytraceShader);
  NullifyResource (myGlContext, myPostFSAAShader);

  NullifyResource (myGlContext, myRaytraceProgram);
  NullifyResource (myGlContext, myPostFSAAProgram);

  NullifyResource (myGlContext, mySceneNodeInfoTexture);
  NullifyResource (myGlContext, mySceneMinPointTexture);
  NullifyResource (myGlContext, mySceneMaxPointTexture);

  NullifyResource (myGlContext, myObjectNodeInfoTexture);
  NullifyResource (myGlContext, myObjectMinPointTexture);
  NullifyResource (myGlContext, myObjectMaxPointTexture);

  NullifyResource (myGlContext, myGeometryVertexTexture);
  NullifyResource (myGlContext, myGeometryNormalTexture);
  NullifyResource (myGlContext, myGeometryTriangTexture);

  NullifyResource (myGlContext, myRaytraceLightSrcTexture);
  NullifyResource (myGlContext, myRaytraceMaterialTexture);

  if (myRaytraceScreenQuad.IsValid())
    myRaytraceScreenQuad.Release (myGlContext.operator->());
}

// =======================================================================
// function : UploadRaytraceData
// purpose  : Uploads ray-trace data to the GPU
// =======================================================================
Standard_Boolean OpenGl_Workspace::UploadRaytraceData()
{
  if (!myGlContext->IsGlGreaterEqual (3, 1))
  {
#ifdef RAY_TRACE_PRINT_INFO
    std::cout << "Error: OpenGL version is less than 3.1" << std::endl;
#endif
    return Standard_False;
  }

  /////////////////////////////////////////////////////////////////////////////
  // Create OpenGL texture buffers

  if (mySceneNodeInfoTexture.IsNull())  // create hight-level BVH buffers
  {
    mySceneNodeInfoTexture = new OpenGl_TextureBufferArb;
    mySceneMinPointTexture = new OpenGl_TextureBufferArb;
    mySceneMaxPointTexture = new OpenGl_TextureBufferArb;

    if (!mySceneNodeInfoTexture->Create (myGlContext)
      || !mySceneMinPointTexture->Create (myGlContext)
      || !mySceneMaxPointTexture->Create (myGlContext))
    {
#ifdef RAY_TRACE_PRINT_INFO
      std::cout << "Error: Failed to create buffers for high-level scene BVH" << std::endl;
#endif
      return Standard_False;
    }
  }

  if (myObjectNodeInfoTexture.IsNull())  // create bottom-level BVH buffers
  {
    myObjectNodeInfoTexture = new OpenGl_TextureBufferArb;
    myObjectMinPointTexture = new OpenGl_TextureBufferArb;
    myObjectMaxPointTexture = new OpenGl_TextureBufferArb;

    if (!myObjectNodeInfoTexture->Create (myGlContext)
      || !myObjectMinPointTexture->Create (myGlContext)
      || !myObjectMaxPointTexture->Create (myGlContext))
    {
#ifdef RAY_TRACE_PRINT_INFO
      std::cout << "Error: Failed to create buffers for bottom-level scene BVH" << std::endl;
#endif
      return Standard_False;
    }
  }

  if (myGeometryVertexTexture.IsNull())  // create geometry buffers
  {
    myGeometryVertexTexture = new OpenGl_TextureBufferArb;
    myGeometryNormalTexture = new OpenGl_TextureBufferArb;
    myGeometryTriangTexture = new OpenGl_TextureBufferArb;

    if (!myGeometryVertexTexture->Create (myGlContext)
      || !myGeometryNormalTexture->Create (myGlContext)
      || !myGeometryTriangTexture->Create (myGlContext))
    {
#ifdef RAY_TRACE_PRINT_INFO
      std::cout << "Error: Failed to create buffers for triangulation data" << std::endl;
#endif
      return Standard_False;
    }
  }

  if (myRaytraceMaterialTexture.IsNull())  // create material buffer
  {
    myRaytraceMaterialTexture = new OpenGl_TextureBufferArb;

    if (!myRaytraceMaterialTexture->Create (myGlContext))
    {
#ifdef RAY_TRACE_PRINT_INFO
      std::cout << "Error: Failed to create buffers for material data" << std::endl;
#endif
      return Standard_False;
    }
  }

  /////////////////////////////////////////////////////////////////////////////
  // Write OpenGL texture buffers

  const NCollection_Handle<BVH_Tree<Standard_ShortReal, 4> >& aBVH = myRaytraceGeometry.BVH();

  bool aResult = true;
  if (!aBVH->NodeInfoBuffer().empty())
  {
    aResult &= mySceneNodeInfoTexture->Init (myGlContext, 4, GLsizei (aBVH->NodeInfoBuffer().size()),
                                             reinterpret_cast<const GLuint*> (&aBVH->NodeInfoBuffer().front()));
    aResult &= mySceneMinPointTexture->Init (myGlContext, 4, GLsizei (aBVH->MinPointBuffer().size()),
                                             reinterpret_cast<const GLfloat*> (&aBVH->MinPointBuffer().front()));
    aResult &= mySceneMaxPointTexture->Init (myGlContext, 4, GLsizei (aBVH->MaxPointBuffer().size()),
                                             reinterpret_cast<const GLfloat*> (&aBVH->MaxPointBuffer().front()));
  }
  if (!aResult)
  {
#ifdef RAY_TRACE_PRINT_INFO
    std::cout << "Error: Failed to upload buffers for high-level scene BVH" << std::endl;
#endif
    return Standard_False;
  }

  Standard_Size aTotalVerticesNb = 0;
  Standard_Size aTotalElementsNb = 0;
  Standard_Size aTotalBVHNodesNb = 0;

  for (Standard_Integer anElemIndex = 0; anElemIndex < myRaytraceGeometry.Size(); ++anElemIndex)
  {
    OpenGl_TriangleSet* aTriangleSet = dynamic_cast<OpenGl_TriangleSet*> (
      myRaytraceGeometry.Objects().ChangeValue (anElemIndex).operator->());

    Standard_ASSERT_RETURN (aTriangleSet != NULL,
      "Error: Failed to get triangulation of OpenGL element", Standard_False);

    aTotalVerticesNb += aTriangleSet->Vertices.size();
    aTotalElementsNb += aTriangleSet->Elements.size();

    Standard_ASSERT_RETURN (!aTriangleSet->BVH().IsNull(),
      "Error: Failed to get bottom-level BVH of OpenGL element", Standard_False);

    aTotalBVHNodesNb += aTriangleSet->BVH()->NodeInfoBuffer().size();
  }

  if (aTotalBVHNodesNb != 0)
  {
    aResult &= myObjectNodeInfoTexture->Init (myGlContext, 4, GLsizei (aTotalBVHNodesNb), static_cast<const GLuint*>  (NULL));
    aResult &= myObjectMinPointTexture->Init (myGlContext, 4, GLsizei (aTotalBVHNodesNb), static_cast<const GLfloat*> (NULL));
    aResult &= myObjectMaxPointTexture->Init (myGlContext, 4, GLsizei (aTotalBVHNodesNb), static_cast<const GLfloat*> (NULL));
  }

  if (!aResult)
  {
#ifdef RAY_TRACE_PRINT_INFO
    std::cout << "Error: Failed to upload buffers for bottom-level scene BVH" << std::endl;
#endif
    return Standard_False;
  }

  if (aTotalElementsNb != 0)
  {
    aResult &= myGeometryTriangTexture->Init (myGlContext, 4, GLsizei (aTotalElementsNb), static_cast<const GLuint*> (NULL));
  }

  if (aTotalVerticesNb != 0)
  {
    aResult &= myGeometryVertexTexture->Init (myGlContext, 4, GLsizei (aTotalVerticesNb), static_cast<const GLfloat*> (NULL));
    aResult &= myGeometryNormalTexture->Init (myGlContext, 4, GLsizei (aTotalVerticesNb), static_cast<const GLfloat*> (NULL));
  }

  if (!aResult)
  {
#ifdef RAY_TRACE_PRINT_INFO
    std::cout << "Error: Failed to upload buffers for scene geometry" << std::endl;
#endif
    return Standard_False;
  }

  for (Standard_Integer aNodeIdx = 0; aNodeIdx < aBVH->Length(); ++aNodeIdx)
  {
    if (!aBVH->IsOuter (aNodeIdx))
      continue;

    OpenGl_TriangleSet* aTriangleSet = myRaytraceGeometry.TriangleSet (aNodeIdx);

    Standard_ASSERT_RETURN (aTriangleSet != NULL,
      "Error: Failed to get triangulation of OpenGL element", Standard_False);

    const Standard_Integer aBVHOffset = myRaytraceGeometry.AccelerationOffset (aNodeIdx);

    Standard_ASSERT_RETURN (aBVHOffset != OpenGl_RaytraceGeometry::INVALID_OFFSET,
      "Error: Failed to get offset for bottom-level BVH", Standard_False);

    const size_t aBVHBuffserSize = aTriangleSet->BVH()->NodeInfoBuffer().size();

    if (aBVHBuffserSize != 0)
    {
      aResult &= myObjectNodeInfoTexture->SubData (myGlContext, aBVHOffset, GLsizei (aBVHBuffserSize),
                                                   reinterpret_cast<const GLuint*> (&aTriangleSet->BVH()->NodeInfoBuffer().front()));
      aResult &= myObjectMinPointTexture->SubData (myGlContext, aBVHOffset, GLsizei (aBVHBuffserSize),
                                                   reinterpret_cast<const GLfloat*> (&aTriangleSet->BVH()->MinPointBuffer().front()));
      aResult &= myObjectMaxPointTexture->SubData (myGlContext, aBVHOffset, GLsizei (aBVHBuffserSize),
                                                   reinterpret_cast<const GLfloat*> (&aTriangleSet->BVH()->MaxPointBuffer().front()));
      if (!aResult)
      {
#ifdef RAY_TRACE_PRINT_INFO
        std::cout << "Error: Failed to upload buffers for bottom-level scene BVHs" << std::endl;
#endif
        return Standard_False;
      }
    }

    const Standard_Integer aVerticesOffset = myRaytraceGeometry.VerticesOffset (aNodeIdx);

    Standard_ASSERT_RETURN (aVerticesOffset != OpenGl_RaytraceGeometry::INVALID_OFFSET,
      "Error: Failed to get offset for triangulation vertices of OpenGL element", Standard_False);

    if (!aTriangleSet->Vertices.empty())
    {
      aResult &= myGeometryNormalTexture->SubData (myGlContext, aVerticesOffset, GLsizei (aTriangleSet->Normals.size()),
                                                   reinterpret_cast<const GLfloat*> (&aTriangleSet->Normals.front()));
      aResult &= myGeometryVertexTexture->SubData (myGlContext, aVerticesOffset, GLsizei (aTriangleSet->Vertices.size()),
                                                   reinterpret_cast<const GLfloat*> (&aTriangleSet->Vertices.front()));
    }

    const Standard_Integer anElementsOffset = myRaytraceGeometry.ElementsOffset (aNodeIdx);

    Standard_ASSERT_RETURN (anElementsOffset != OpenGl_RaytraceGeometry::INVALID_OFFSET,
      "Error: Failed to get offset for triangulation elements of OpenGL element", Standard_False);

    if (!aTriangleSet->Elements.empty())
    {
      aResult &= myGeometryTriangTexture->SubData (myGlContext, anElementsOffset, GLsizei (aTriangleSet->Elements.size()),
                                                   reinterpret_cast<const GLuint*> (&aTriangleSet->Elements.front()));
    }

    if (!aResult)
    {
#ifdef RAY_TRACE_PRINT_INFO
      std::cout << "Error: Failed to upload triangulation buffers for OpenGL element" << std::endl;
#endif
      return Standard_False;
    }
  }

  if (myRaytraceGeometry.Materials.size() != 0)
  {
    const GLfloat* aDataPtr = myRaytraceGeometry.Materials.front().Packed();
    aResult &= myRaytraceMaterialTexture->Init (myGlContext, 4, GLsizei (myRaytraceGeometry.Materials.size() * 7), aDataPtr);
    if (!aResult)
    {
#ifdef RAY_TRACE_PRINT_INFO
      std::cout << "Error: Failed to upload material buffer" << std::endl;
#endif
      return Standard_False;
    }
  }

  myIsRaytraceDataValid = myRaytraceGeometry.Objects().Size() != 0;

#ifdef RAY_TRACE_PRINT_INFO

  Standard_ShortReal aMemUsed = 0.f;

  for (Standard_Integer anElemIdx = 0; anElemIdx < myRaytraceGeometry.Size(); ++anElemIdx)
  {
    OpenGl_TriangleSet* aTriangleSet = dynamic_cast<OpenGl_TriangleSet*> (
      myRaytraceGeometry.Objects().ChangeValue (anElemIdx).operator->());

    aMemUsed += static_cast<Standard_ShortReal> (
      aTriangleSet->Vertices.size() * sizeof (BVH_Vec4f));
    aMemUsed += static_cast<Standard_ShortReal> (
      aTriangleSet->Normals.size() * sizeof (BVH_Vec4f));
    aMemUsed += static_cast<Standard_ShortReal> (
      aTriangleSet->Elements.size() * sizeof (BVH_Vec4i));

    aMemUsed += static_cast<Standard_ShortReal> (
      aTriangleSet->BVH()->NodeInfoBuffer().size() * sizeof (BVH_Vec4i));
    aMemUsed += static_cast<Standard_ShortReal> (
      aTriangleSet->BVH()->MinPointBuffer().size() * sizeof (BVH_Vec4f));
    aMemUsed += static_cast<Standard_ShortReal> (
      aTriangleSet->BVH()->MaxPointBuffer().size() * sizeof (BVH_Vec4f));
  }

  aMemUsed += static_cast<Standard_ShortReal> (
    myRaytraceGeometry.BVH()->NodeInfoBuffer().size() * sizeof (BVH_Vec4i));
  aMemUsed += static_cast<Standard_ShortReal> (
    myRaytraceGeometry.BVH()->MinPointBuffer().size() * sizeof (BVH_Vec4f));
  aMemUsed += static_cast<Standard_ShortReal> (
    myRaytraceGeometry.BVH()->MaxPointBuffer().size() * sizeof (BVH_Vec4f));

  std::cout << "GPU Memory Used (MB): ~" << aMemUsed / 1048576 << std::endl;

#endif

  return aResult;
}

// =======================================================================
// function : ResizeRaytraceBuffers
// purpose  : Resizes OpenGL frame buffers
// =======================================================================
Standard_Boolean OpenGl_Workspace::ResizeRaytraceBuffers (const Standard_Integer theSizeX,
                                                          const Standard_Integer theSizeY)
{
  if (myRaytraceFBO1->GetVPSizeX() != theSizeX
   || myRaytraceFBO1->GetVPSizeY() != theSizeY)
  {
    myRaytraceFBO1->Init (myGlContext, theSizeX, theSizeY);
    myRaytraceFBO2->Init (myGlContext, theSizeX, theSizeY);
  }

  return Standard_True;
}

// =======================================================================
// function : UpdateCamera
// purpose  : Generates viewing rays for corners of screen quad
// =======================================================================
void OpenGl_Workspace::UpdateCamera (const NCollection_Mat4<GLdouble>& theOrientation,
                                     const NCollection_Mat4<GLdouble>& theViewMapping,
                                     OpenGl_Vec3                       theOrigins[4],
                                     OpenGl_Vec3                       theDirects[4])
{
  NCollection_Mat4<GLdouble> aInvModelProj;

  // compute invserse model-view-projection matrix
  (theViewMapping * theOrientation).Inverted (aInvModelProj);

  Standard_Integer aOriginIndex = 0;
  Standard_Integer aDirectIndex = 0;

  for (Standard_Integer aY = -1; aY <= 1; aY += 2)
  {
    for (Standard_Integer aX = -1; aX <= 1; aX += 2)
    {
      OpenGl_Vec4d aOrigin (GLdouble(aX),
                            GLdouble(aY),
                           -1.0,
                            1.0);

      aOrigin = aInvModelProj * aOrigin;

      aOrigin.x() = aOrigin.x() / aOrigin.w();
      aOrigin.y() = aOrigin.y() / aOrigin.w();
      aOrigin.z() = aOrigin.z() / aOrigin.w();

      OpenGl_Vec4d aDirect (GLdouble(aX),
                            GLdouble(aY),
                            1.0,
                            1.0);

      aDirect = aInvModelProj * aDirect;

      aDirect.x() = aDirect.x() / aDirect.w();
      aDirect.y() = aDirect.y() / aDirect.w();
      aDirect.z() = aDirect.z() / aDirect.w();

      aDirect = aDirect - aOrigin;

      GLdouble aInvLen = 1.0 / sqrt (aDirect.x() * aDirect.x() +
                                     aDirect.y() * aDirect.y() +
                                     aDirect.z() * aDirect.z());

      theOrigins[aOriginIndex++] = OpenGl_Vec3 (static_cast<GLfloat> (aOrigin.x()),
                                                static_cast<GLfloat> (aOrigin.y()),
                                                static_cast<GLfloat> (aOrigin.z()));

      theDirects[aDirectIndex++] = OpenGl_Vec3 (static_cast<GLfloat> (aDirect.x() * aInvLen),
                                                static_cast<GLfloat> (aDirect.y() * aInvLen),
                                                static_cast<GLfloat> (aDirect.z() * aInvLen));
    }
  }
}

// =======================================================================
// function : RunRaytraceShaders
// purpose  : Runs ray-tracing shader programs
// =======================================================================
Standard_Boolean OpenGl_Workspace::RunRaytraceShaders (const Graphic3d_CView& theCView,
                                                       const Standard_Integer theSizeX,
                                                       const Standard_Integer theSizeY,
                                                       const OpenGl_Vec3      theOrigins[4],
                                                       const OpenGl_Vec3      theDirects[4],
                                                       OpenGl_FrameBuffer*    theFrameBuffer)
{
  mySceneMinPointTexture->BindTexture (myGlContext, GL_TEXTURE0 + OpenGl_RT_SceneMinPointTexture);
  mySceneMaxPointTexture->BindTexture (myGlContext, GL_TEXTURE0 + OpenGl_RT_SceneMaxPointTexture);
  mySceneNodeInfoTexture->BindTexture (myGlContext, GL_TEXTURE0 + OpenGl_RT_SceneNodeInfoTexture);
  myObjectMinPointTexture->BindTexture (myGlContext, GL_TEXTURE0 + OpenGl_RT_ObjectMinPointTexture);
  myObjectMaxPointTexture->BindTexture (myGlContext, GL_TEXTURE0 + OpenGl_RT_ObjectMaxPointTexture);
  myObjectNodeInfoTexture->BindTexture (myGlContext, GL_TEXTURE0 + OpenGl_RT_ObjectNodeInfoTexture);
  myGeometryVertexTexture->BindTexture (myGlContext, GL_TEXTURE0 + OpenGl_RT_GeometryVertexTexture);
  myGeometryNormalTexture->BindTexture (myGlContext, GL_TEXTURE0 + OpenGl_RT_GeometryNormalTexture);
  myGeometryTriangTexture->BindTexture (myGlContext, GL_TEXTURE0 + OpenGl_RT_GeometryTriangTexture);
  myRaytraceMaterialTexture->BindTexture (myGlContext, GL_TEXTURE0 + OpenGl_RT_RaytraceMaterialTexture);
  myRaytraceLightSrcTexture->BindTexture (myGlContext, GL_TEXTURE0 + OpenGl_RT_RaytraceLightSrcTexture);

  if (theCView.IsAntialiasingEnabled) // render source image to FBO
  {
    myRaytraceFBO1->BindBuffer (myGlContext);
    
    glDisable (GL_BLEND);
  }

  myRaytraceProgram->Bind (myGlContext);

  Standard_Integer aLightSourceBufferSize =
    static_cast<Standard_Integer> (myRaytraceGeometry.Sources.size());

  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uOriginLB], theOrigins[0]);
  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uOriginRB], theOrigins[1]);
  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uOriginLT], theOrigins[2]);
  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uOriginRT], theOrigins[3]);
  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uDirectLB], theDirects[0]);
  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uDirectRB], theDirects[1]);
  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uDirectLT], theDirects[2]);
  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uDirectRT], theDirects[3]);
  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uSceneRad], myRaytraceSceneRadius);
  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uSceneEps], myRaytraceSceneEpsilon);
  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uLightCount], aLightSourceBufferSize);
  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uLightAmbnt], myRaytraceGeometry.Ambient);
  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uShadEnabled], theCView.IsShadowsEnabled);
  myRaytraceProgram->SetUniform (myGlContext,
    myUniformLocations[0][OpenGl_RT_uReflEnabled], theCView.IsReflectionsEnabled);

  myGlContext->core20fwd->glEnableVertexAttribArray (
    myUniformLocations[0][OpenGl_RT_aPosition]);
  {
    myGlContext->core20fwd->glVertexAttribPointer (
      myUniformLocations[0][OpenGl_RT_aPosition], 3, GL_FLOAT, GL_FALSE, 0, NULL);

    myGlContext->core15fwd->glDrawArrays (GL_TRIANGLES, 0, 6);
  }
  myGlContext->core20fwd->glDisableVertexAttribArray (
    myUniformLocations[0][OpenGl_RT_aPosition]);
  
  if (!theCView.IsAntialiasingEnabled)
  {
    myRaytraceProgram->Unbind (myGlContext);

    return Standard_True;
  }

  myGlContext->core20fwd->glActiveTexture (
    GL_TEXTURE0 + OpenGl_RT_FSAAInputTexture); // texture unit for FBO texture

  myRaytraceFBO1->BindTexture (myGlContext);

  myPostFSAAProgram->Bind (myGlContext);

  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uOriginLB], theOrigins[0]);
  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uOriginRB], theOrigins[1]);
  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uOriginLT], theOrigins[2]);
  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uOriginRT], theOrigins[3]);
  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uDirectLB], theDirects[0]);
  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uDirectRB], theDirects[1]);
  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uDirectLT], theDirects[2]);
  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uDirectRT], theDirects[3]);
  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uSceneRad], myRaytraceSceneRadius);
  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uSceneEps], myRaytraceSceneEpsilon);
  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uLightCount], aLightSourceBufferSize);
  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uLightAmbnt], myRaytraceGeometry.Ambient);
  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uShadEnabled], theCView.IsShadowsEnabled);
  myPostFSAAProgram->SetUniform (myGlContext,
    myUniformLocations[1][OpenGl_RT_uReflEnabled], theCView.IsReflectionsEnabled);

  const Standard_ShortReal aMaxOffset = 0.559017f;
  const Standard_ShortReal aMinOffset = 0.186339f;

  myGlContext->core20fwd->glEnableVertexAttribArray (
    myUniformLocations[1][OpenGl_RT_aPosition]);
  
  myGlContext->core20fwd->glVertexAttribPointer (
    myUniformLocations[1][OpenGl_RT_aPosition], 3, GL_FLOAT, GL_FALSE, 0, NULL);

  // Perform multi-pass adaptive FSAA using ping-pong technique
  for (Standard_Integer anIt = 0; anIt < 4; ++anIt)
  {
    GLfloat aOffsetX = 1.f / theSizeX;
    GLfloat aOffsetY = 1.f / theSizeY;

    if (anIt < 2)
    {
      aOffsetX *= anIt < 1 ? aMinOffset : -aMaxOffset;
      aOffsetY *= anIt < 1 ? aMaxOffset :  aMinOffset;
    }
    else
    {
      aOffsetX *= anIt > 2 ?  aMaxOffset : -aMinOffset;
      aOffsetY *= anIt > 2 ? -aMinOffset : -aMaxOffset;
    }
    
    myPostFSAAProgram->SetUniform (myGlContext,
      myUniformLocations[1][OpenGl_RT_uSamples], anIt + 2);
    myPostFSAAProgram->SetUniform (myGlContext,
      myUniformLocations[1][OpenGl_RT_uOffsetX], aOffsetX);
    myPostFSAAProgram->SetUniform (myGlContext,
      myUniformLocations[1][OpenGl_RT_uOffsetY], aOffsetY);

    Handle(OpenGl_FrameBuffer)& aFramebuffer = anIt % 2 ? myRaytraceFBO1 : myRaytraceFBO2;

    if (anIt == 3) // disable FBO on last iteration
    {
      glEnable (GL_BLEND);

      if (theFrameBuffer != NULL)
        theFrameBuffer->BindBuffer (myGlContext);
    }
    else
    {
      aFramebuffer->BindBuffer (myGlContext);
    }
    
    myGlContext->core15fwd->glDrawArrays (GL_TRIANGLES, 0, 6);

    if (anIt != 3) // set input for the next pass
    {
      aFramebuffer->BindTexture  (myGlContext);
      aFramebuffer->UnbindBuffer (myGlContext);
    }
  }

  myGlContext->core20fwd->glDisableVertexAttribArray (
    myUniformLocations[1][OpenGl_RT_aPosition]);

  myPostFSAAProgram->Unbind (myGlContext);

  return Standard_True;
}

// =======================================================================
// function : Raytrace
// purpose  : Redraws the window using OpenGL/GLSL ray-tracing
// =======================================================================
Standard_Boolean OpenGl_Workspace::Raytrace (const Graphic3d_CView& theCView,
                                             const Standard_Integer theSizeX,
                                             const Standard_Integer theSizeY,
                                             const Standard_Boolean theToSwap,
                                             OpenGl_FrameBuffer*    theFrameBuffer)
{
  if (!UpdateRaytraceGeometry (Standard_True))
    return Standard_False;

  if (!InitRaytraceResources())
    return Standard_False;

  if (!ResizeRaytraceBuffers (theSizeX, theSizeY))
    return Standard_False;

  if (!UpdateRaytraceEnvironmentMap())
    return Standard_False;

  // Get model-view and projection matrices
  TColStd_Array2OfReal theOrientation (0, 3, 0, 3);
  TColStd_Array2OfReal theViewMapping (0, 3, 0, 3);

  myView->GetMatrices (theOrientation, theViewMapping);

  NCollection_Mat4<GLdouble> aOrientationMatrix;
  NCollection_Mat4<GLdouble> aViewMappingMatrix;

  for (Standard_Integer j = 0; j < 4; ++j)
  {
    for (Standard_Integer i = 0; i < 4; ++i)
    {
      aOrientationMatrix [4 * j + i] = theOrientation (i, j);
      aViewMappingMatrix [4 * j + i] = theViewMapping (i, j);
    }
  }
  
  NCollection_Mat4<GLdouble> aInvOrientationMatrix;
  aOrientationMatrix.Inverted (aInvOrientationMatrix);

  if (!UpdateRaytraceLightSources (aInvOrientationMatrix))
    return Standard_False;

  OpenGl_Vec3 aOrigins[4];
  OpenGl_Vec3 aDirects[4];

  UpdateCamera (aOrientationMatrix,
                aViewMappingMatrix,
                aOrigins,
                aDirects);

  // Draw background
  glPushAttrib (GL_ENABLE_BIT |
                GL_CURRENT_BIT |
                GL_COLOR_BUFFER_BIT |
                GL_DEPTH_BUFFER_BIT);

  glDisable (GL_DEPTH_TEST);

  if (NamedStatus & OPENGL_NS_WHITEBACK)
  {
    glClearColor (1.0f, 1.0f, 1.0f, 1.0f);
  }
  else
  {
    glClearColor (myBgColor.rgb[0],
                  myBgColor.rgb[1],
                  myBgColor.rgb[2],
                  1.0f);
  }

  glClear (GL_COLOR_BUFFER_BIT);

  if (theFrameBuffer != NULL)
    theFrameBuffer->BindBuffer (myGlContext);

  myView->DrawBackground (*this);

  // Generate ray-traced image
  glMatrixMode (GL_PROJECTION);
  glLoadIdentity();

  glMatrixMode (GL_MODELVIEW);
  glLoadIdentity();

  glEnable (GL_BLEND);
  glBlendFunc (GL_ONE, GL_SRC_ALPHA);

  if (myIsRaytraceDataValid)
  {
    myRaytraceScreenQuad.Bind (myGlContext);

    RunRaytraceShaders (theCView,
                        theSizeX,
                        theSizeY,
                        aOrigins,
                        aDirects,
                        theFrameBuffer);

    myRaytraceScreenQuad.Unbind (myGlContext);
  }

  if (theFrameBuffer != NULL)
    theFrameBuffer->UnbindBuffer (myGlContext);

  glPopAttrib();

  // Swap the buffers
  if (theToSwap)
  {
    GetGlContext()->SwapBuffers();
    myBackBufferRestored = Standard_False;
  }
  else
  {
    glFlush();
  }

  return Standard_True;
}
